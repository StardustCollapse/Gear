#include "Compiler.h"
#include "Parser.h"
#include "Generator.h"
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <set>
#include <stack>

namespace Gen = Gear::Generator;

// ============================================================
// Token cursor — walks the flat token stream directly (no AST)
// ============================================================
static const std::vector<TOKEN>* g_pTokens = nullptr;
static size_t g_Pos = 0;
static bool g_InBracketDeref = false;
static std::string g_LastFuncName;

static const TOKEN& Peek(int offset = 0)
{
	static TOKEN s_EOF = { "", 0, 0, 0, (uint8_t)NONE };
	size_t idx = g_Pos + offset;
	if (idx >= g_pTokens->size()) return s_EOF;
	return (*g_pTokens)[idx];
}

static const TOKEN& Advance()
{
	return (*g_pTokens)[g_Pos++];
}

static bool AtEnd()
{
	return g_Pos >= g_pTokens->size();
}

static bool Check(uint8_t type, const std::string& name)
{
	if (AtEnd()) return false;
	const TOKEN& t = Peek();
	return t.type == type && t.name == name;
}

static bool CheckOp(uint16_t op)
{
	if (AtEnd()) return false;
	return Peek().type == OPERATOR && Peek().op == op;
}

static void ExpectOp(uint16_t op, const std::string& context)
{
	if (AtEnd() || Peek().type != OPERATOR || Peek().op != op)
	{
		uint32_t line = AtEnd() ? 0 : Peek().line;
		throw std::runtime_error("Error: Expected '" + context + "' at line " + std::to_string(line));
	}
	Advance();
}

// ============================================================
// State per function scope
// ============================================================
static FUNC_INFO g_CurrentFunc;
static std::unordered_map<std::string, VAR_INFO> g_Locals;
static std::vector<NESTED_FUNC> g_AllNestedFuncs;

static std::stack<std::string> g_LoopEndLabels;
static std::string g_ReturnLabel;

// ============================================================
// Windows x64 register size mapping
// ============================================================
static std::string RegForSize(const std::string& reg64, uint8_t size)
{
	if (reg64 == "rax") { switch (size) { case 1: return "al";   case 2: return "ax";   case 4: return "eax";  default: return "rax"; } }
	if (reg64 == "rcx") { switch (size) { case 1: return "cl";   case 2: return "cx";   case 4: return "ecx";  default: return "rcx"; } }
	if (reg64 == "rdx") { switch (size) { case 1: return "dl";   case 2: return "dx";   case 4: return "edx";  default: return "rdx"; } }
	if (reg64 == "r10") { switch (size) { case 1: return "r10b"; case 2: return "r10w"; case 4: return "r10d"; default: return "r10"; } }
	if (reg64 == "r11") { switch (size) { case 1: return "r11b"; case 2: return "r11w"; case 4: return "r11d"; default: return "r11"; } }
	if (reg64 == "r8")  { switch (size) { case 1: return "r8b";  case 2: return "r8w";  case 4: return "r8d";  default: return "r8";  } }
	if (reg64 == "r9")  { switch (size) { case 1: return "r9b";  case 2: return "r9w";  case 4: return "r9d";  default: return "r9";  } }
	return reg64;
}

static std::string SizeDirective(uint8_t size)
{
	switch (size)
	{
	case 1: return "BYTE PTR";
	case 2: return "WORD PTR";
	case 4: return "DWORD PTR";
	case 8: return "QWORD PTR";
	default: return "QWORD PTR";
	}
}

// Emit x86-64 compliant zero-extending memory load
static void EmitLoadMem(const std::string& reg64, uint8_t size, const std::string& mem)
{
	if (size == 1)
		Gen::EmitLine("movzx " + RegForSize(reg64, 4) + ", BYTE PTR " + mem);
	else if (size == 2)
		Gen::EmitLine("movzx " + RegForSize(reg64, 4) + ", WORD PTR " + mem);
	else if (size == 4)
		Gen::EmitLine("mov " + RegForSize(reg64, 4) + ", DWORD PTR " + mem);
	else
		Gen::EmitLine("mov " + reg64 + ", QWORD PTR " + mem);
}

// ============================================================
// Allocate a local variable on stack frame
// ============================================================
static VAR_INFO AllocLocal(const std::string& name, uint8_t size)
{
	g_CurrentFunc.next_offset -= size;
	while ((-g_CurrentFunc.next_offset) % size != 0)
		g_CurrentFunc.next_offset--;

	VAR_INFO var;
	var.name = name;
	var.size = size;
	var.rbp_offset = g_CurrentFunc.next_offset;
	g_Locals[name] = var;
	return var;
}

static uint8_t g_LastExprSize = 0;

// ============================================================
// Struct definition and field tracking
// ============================================================
struct STRUCT_FIELD {
	std::string name;
	uint8_t size;
	uint16_t offset;
};

struct STRUCT_DEF {
	std::string name;
	std::vector<STRUCT_FIELD> fields;
	uint16_t total_size;
};

static std::unordered_map<std::string, STRUCT_DEF> g_StructDefs;

static const STRUCT_FIELD* FindStructField(const std::string& field_name, const STRUCT_DEF** out_struct = nullptr)
{
	for (auto& pair : g_StructDefs)
	{
		for (auto& f : pair.second.fields)
		{
			if (f.name == field_name)
			{
				if (out_struct) *out_struct = &pair.second;
				return &f;
			}
		}
	}
	return nullptr;
}

static void EmitLoadBasePtr(const std::string& reg64, const std::string& name)
{
	auto it = g_Locals.find(name);
	if (it != g_Locals.end())
	{
		EmitLoadMem(reg64, it->second.size, "[rbp" + std::to_string(it->second.rbp_offset) + "]");
	}
	else if (Gen::IsGlobalVar(name))
	{
		Gen::EmitLine("mov " + reg64 + ", QWORD PTR [" + name + "]");
	}
	else
	{
		Gen::EmitLine("lea " + reg64 + ", [" + name + "]");
	}
}

static void ParseStructDefinition()
{
	Advance(); // consume 'struct'
	if (Peek().type != IDENTIFIER)
	{
		throw std::runtime_error("Error: Expected struct name at line " + std::to_string(Peek().line));
	}
	std::string struct_name = Peek().name;
	Advance();

	ExpectOp('{', "{");
	STRUCT_DEF sdef;
	sdef.name = struct_name;
	sdef.total_size = 0;

	while (!AtEnd() && !CheckOp('}'))
	{
		while (!AtEnd() && (CheckOp(';') || CheckOp(','))) Advance();
		if (CheckOp('}')) break;

		if (Peek().type != IDENTIFIER)
		{
			throw std::runtime_error("Error: Expected field name in struct '" + struct_name + "' at line " + std::to_string(Peek().line));
		}
		std::string field_name = Peek().name;
		Advance();

		ExpectOp(':', ":");
		if (Peek().type != CONSTANT)
		{
			throw std::runtime_error("Error: Expected field size in struct '" + struct_name + "' at line " + std::to_string(Peek().line));
		}
		uint8_t field_size = (uint8_t)Peek().value;
		Advance();

		STRUCT_FIELD f;
		f.name = field_name;
		f.size = field_size;
		f.offset = sdef.total_size;
		sdef.fields.push_back(f);
		sdef.total_size += field_size;

		while (!AtEnd() && (CheckOp(';') || CheckOp(','))) Advance();
	}

	ExpectOp('}', "}");
	g_StructDefs[struct_name] = sdef;
}

// ============================================================
// Angle bracket syntax: <IDENTIFIER>
// Semantic: Access the original data of function parameter 'name' (dereferencing the pointer)
// Fallback: Access global variable if not a local parameter
// ============================================================
static bool IsAngleRef()
{
	return CheckOp('<') &&
		g_Pos + 2 < g_pTokens->size() &&
		(*g_pTokens)[g_Pos + 1].type == IDENTIFIER &&
		(*g_pTokens)[g_Pos + 2].type == OPERATOR &&
		(*g_pTokens)[g_Pos + 2].op == '>';
}

static std::string ParseAngleRefName()
{
	ExpectOp('<', "<");
	std::string name = Peek().name;
	Advance();
	ExpectOp('>', ">");
	return name;
}

// ============================================================
// Forward declarations
// ============================================================
static void ParseExpression();
static void ParseStatement();
static void ParseBlock();
static void CollectNestedBlocks();
static void ParseThenChain();

// ============================================================
// Expression parsing — recursive descent directly emitting assembly
// The evaluation result is always placed in RAX
// ============================================================

static void ParsePrimary()
{
	// Parenthesized expression: ( expr )
	if (CheckOp('('))
	{
		Advance();
		ParseExpression();
		ExpectOp(')', ")");
		return;
	}

	// Negative constant: -NUMBER
	if (CheckOp('-') && g_Pos + 1 < g_pTokens->size() && Peek(1).type == CONSTANT)
	{
		Advance();
		int64_t val = -(int64_t)Peek().value;
		Advance();
		Gen::EmitLine("mov rax, " + std::to_string(val));
		return;
	}

	// Angle bracket access: <a> or <a>:size
	// Semantic: Access original data in caller environment (bypassing local copy)
	if (IsAngleRef())
	{
		std::string name = ParseAngleRefName();

		uint8_t size = 8;
		if (CheckOp(':'))
		{
			Advance();
			size = (uint8_t)Peek().value;
			Advance();
		}

		auto it_orig = g_Locals.find(name + "$orig");
		if (it_orig != g_Locals.end())
		{
			Gen::EmitComment("access original data of parameter <" + name + ">");
			Gen::EmitLine("mov r10, QWORD PTR [rbp" + std::to_string(it_orig->second.rbp_offset) + "]");
			EmitLoadMem("rax", size, "[r10]");
			return;
		}

		auto it = g_Locals.find(name);
		if (it != g_Locals.end())
		{
			Gen::EmitComment("access data via pointer <" + name + ">");
			Gen::EmitLine("mov r10, QWORD PTR [rbp" + std::to_string(it->second.rbp_offset) + "]");
			EmitLoadMem("rax", size, "[r10]");
			return;
		}
		else
		{
			Gen::EnsureGlobalVar(name);
			Gen::EmitLine("lea rcx, [" + name + "]");
			EmitLoadMem("rax", size, "[rcx]");
			return;
		}
	}

	// Memory access / dereference: [ expr : size ] or [ expr : size ] @ offset
	// Or bare [NAME] to take address: p = [GLOBAL] or func([local])
	if (CheckOp('['))
	{
		Advance();

		bool is_struct_or_indexed = false;
		if (g_Pos + 2 < g_pTokens->size() && (*g_pTokens)[g_Pos + 2].type == OPERATOR)
		{
			uint16_t op2 = (*g_pTokens)[g_Pos + 2].op;
			if (op2 == OBJECT_ACCESS || op2 == '@')
				is_struct_or_indexed = true;
		}

		// Bare [IDENTIFIER]: take address of variable in expression (p = [GLOBAL] or [local]),
		// or direct memory write statement: [name] = expr
		if (!is_struct_or_indexed && Peek().type == IDENTIFIER && g_Pos + 1 < g_pTokens->size() && (*g_pTokens)[g_Pos + 1].type == OPERATOR && (*g_pTokens)[g_Pos + 1].op == ']')
		{
			std::string name = Peek().name;
			Advance(); // skip identifier
			ExpectOp(']', "]");

			// If followed by '=', it's a write statement: [name] = expr
			if (CheckOp('='))
			{
				Advance();
				auto it = g_Locals.find(name);
				if (it != g_Locals.end())
				{
					Gen::EmitComment("write to memory pointed to by local [" + name + "]");
					Gen::EmitLine("mov rcx, QWORD PTR [rbp" + std::to_string(it->second.rbp_offset) + "]");
					ParseExpression();
					Gen::EmitLine("mov QWORD PTR [rcx], rax");
				}
				else
				{
					Gen::EnsureGlobalVar(name);
					Gen::EmitComment("write to global [" + name + "]");
					ParseExpression();
					Gen::EmitLine("mov QWORD PTR [" + name + "], rax");
				}
				return;
			}

			// In expression: p = [GLOBAL] or func([local]) -> take address!
			auto it = g_Locals.find(name);
			if (it != g_Locals.end())
			{
				Gen::EmitComment("take address of local variable [" + name + "]");
				Gen::EmitLine("lea rax, [rbp" + std::to_string(it->second.rbp_offset) + "]");
			}
			else
			{
				Gen::EnsureGlobalVar(name);
				Gen::EmitComment("take address of global variable [" + name + "]");
				Gen::EmitLine("lea rax, [" + name + "]");
			}
			return;
		}

		// Inside [...]: check for [<IDENTIFIER>]
		if (IsAngleRef())
		{
			std::string name = ParseAngleRefName();
			uint8_t size = 8;
			if (CheckOp(':'))
			{
				Advance();
				size = (uint8_t)Peek().value;
				Advance();
			}
			ExpectOp(']', "]");

			auto it_orig = g_Locals.find(name + "$orig");
			int rbp_off = (it_orig != g_Locals.end()) ?
				it_orig->second.rbp_offset :
				(g_Locals.find(name) != g_Locals.end() ? g_Locals[name].rbp_offset : 0);

			// Case A: [<name>]@offset
			if (CheckOp('@'))
			{
				Advance();
				Gen::EmitLine("mov rcx, QWORD PTR [rbp" + std::to_string(rbp_off) + "]");
				Gen::EmitLine("push rcx");
				ParseExpression(); // offset in rax
				Gen::EmitLine("pop rcx");
				if (CheckOp('='))
				{
					Advance();
					Gen::EmitLine("push rcx");
					Gen::EmitLine("push rax");
					ParseExpression();
					Gen::EmitLine("mov rdx, rax");
					Gen::EmitLine("pop rax");
					Gen::EmitLine("pop rcx");
					if (size > 1) Gen::EmitLine("imul rax, " + std::to_string(size));
					Gen::EmitLine("add rcx, rax");
					Gen::EmitLine("mov " + SizeDirective(size) + " [rcx], " + RegForSize("rdx", size));
					return;
				}
				if (size > 1) Gen::EmitLine("imul rax, " + std::to_string(size));
				Gen::EmitLine("add rcx, rax");
				EmitLoadMem("rax", size, "[rcx]");
				return;
			}

			// Case B: [<a>] in expression -> get the address of caller's original data!
			Gen::EmitComment("get address of original data [<" + name + ">]");
			Gen::EmitLine("mov rax, QWORD PTR [rbp" + std::to_string(rbp_off) + "]");
			return;
		}
		else
		{
			bool saved_bracket = g_InBracketDeref;
			g_InBracketDeref = true;
			ParseExpression();
			g_InBracketDeref = saved_bracket;
			Gen::EmitLine("mov rcx, rax");
		}

		uint8_t size = 8;
		if (CheckOp(':'))
		{
			Advance();
			size = (uint8_t)Peek().value;
			Advance();
		}

		ExpectOp(']', "]");

		// Case 1: Struct field dereference: [hdr]->len
		if (!AtEnd() && Peek().type == OPERATOR && Peek().op == OBJECT_ACCESS)
		{
			Advance(); // skip ->
			std::string field_name = Peek().name;
			Advance();
			Gen::EmitComment("[base]->" + field_name);
			EmitLoadMem("rax", size, "[rcx]");
			return;
		}

		// Case 2: Indexed offset access: [...] @ offset
		if (CheckOp('@'))
		{
			Advance();
			Gen::EmitLine("push rcx");
			ParseExpression(); // offset in rax
			Gen::EmitLine("pop rcx");

			// Write: [...:size]@offset = expr
			if (CheckOp('='))
			{
				Advance();
				Gen::EmitLine("push rcx");
				Gen::EmitLine("push rax");
				ParseExpression(); // value in rax
				Gen::EmitLine("mov rdx, rax");
				Gen::EmitLine("pop rax"); // offset
				Gen::EmitLine("pop rcx"); // base
				if (size > 1)
					Gen::EmitLine("imul rax, " + std::to_string(size));
				Gen::EmitLine("add rcx, rax");
				Gen::EmitLine("mov " + SizeDirective(size) + " [rcx], " + RegForSize("rdx", size));
				return;
			}

			// Read: [...:size]@offset
			if (size > 1)
				Gen::EmitLine("imul rax, " + std::to_string(size));
			Gen::EmitLine("add rcx, rax");
			EmitLoadMem("rax", size, "[rcx]");
			return;
		}

		// Case 3: Direct memory write: [b] = expr or [b:size] = expr (改區域地址)
		if (CheckOp('='))
		{
			Advance();
			Gen::EmitLine("push rcx");
			ParseExpression(); // value in rax
			Gen::EmitLine("pop rcx");
			Gen::EmitLine("mov " + SizeDirective(size) + " [rcx], " + RegForSize("rax", size));
			return;
		}

		// Case 4: Direct memory read: [b] or [b:size]
		EmitLoadMem("rax", size, "[rcx]");
		return;
	}

	// Constant number
	if (Peek().type == CONSTANT)
	{
		uint64_t val = Peek().value;
		Advance();
		if (val <= 0x7FFFFFFF)
			Gen::EmitLine("mov eax, " + std::to_string(val));
		else
			Gen::EmitLine("mov rax, " + std::to_string(val));
		return;
	}

	// Identifier (Variable read or slice or member access)
	if (Peek().type == IDENTIFIER)
	{
		std::string name = Peek().name;
		Advance();

		// Function call: name(arg1, arg2, ...)
		if (CheckOp('('))
		{
			Advance();
			std::vector<std::string> arg_regs = { "rcx", "rdx", "r8", "r9" };
			size_t arg_idx = 0;
			static int s_TempArgCounter = 0;

			while (!CheckOp(')'))
			{
				if (Peek().type == IDENTIFIER &&
					(g_Pos + 1 >= g_pTokens->size() ||
					 (*g_pTokens)[g_Pos + 1].op == ',' ||
					 (*g_pTokens)[g_Pos + 1].op == ')'))
				{
					std::string arg_name = Peek().name;
					Advance();
					auto it = g_Locals.find(arg_name);
					if (it != g_Locals.end())
					{
						Gen::EmitComment("pass address of variable " + arg_name);
						Gen::EmitLine("lea rax, [rbp" + std::to_string(it->second.rbp_offset) + "]");
					}
					else
					{
						Gen::EnsureGlobalVar(arg_name);
						Gen::EmitComment("pass address of global " + arg_name);
						Gen::EmitLine("lea rax, [" + arg_name + "]");
					}
				}
				else
				{
					ParseExpression();
					VAR_INFO temp = AllocLocal("__temp_arg_" + std::to_string(s_TempArgCounter++), 8);
					Gen::EmitLine("mov QWORD PTR [rbp" + std::to_string(temp.rbp_offset) + "], rax");
					Gen::EmitLine("lea rax, [rbp" + std::to_string(temp.rbp_offset) + "]");
				}

				Gen::EmitLine("push rax");
				arg_idx++;
				if (CheckOp(',')) Advance();
			}
			ExpectOp(')', ")");

			for (size_t i = 0; i < arg_idx; i++)
			{
				size_t reg_pos = arg_idx - 1 - i;
				if (reg_pos < 4)
					Gen::EmitLine("pop " + arg_regs[reg_pos]);
				else
					Gen::EmitLine("pop r10");
			}

			Gen::EmitLine("sub rsp, 32");
			Gen::EmitLine("call " + name);
			Gen::EmitLine("add rsp, 32");
			return;
		}

		// Array or structure index: name [ expr ]
		if (CheckOp('['))
		{
			Advance();
			ParseExpression(); // index in rax
			ExpectOp(']', "]");

			if (g_Locals.find(name) != g_Locals.end())
			{
				auto& var = g_Locals[name];
				Gen::EmitLine("mov r10, QWORD PTR [rbp" + std::to_string(var.rbp_offset) + "]");
				Gen::EmitLine("add rax, r10");
			}
			// When name is not a local variable, it acts as a struct type tag / view
			// e.g. pkt_hdr[buf_addr] -> rax is already the base pointer (buf_addr)
			return;
		}

		// Field access: name @ offset . field or name @ offset
		if (CheckOp('@'))
		{
			EmitLoadBasePtr("rcx", name);
			Advance(); // skip @
			Gen::EmitLine("push rcx");
			ParseExpression(); // index/offset in rax
			Gen::EmitLine("pop rcx");

			if (CheckOp('.'))
			{
				Advance(); // skip .
				std::string field = Peek().name;
				Advance();
				const STRUCT_DEF* sdef = nullptr;
				const STRUCT_FIELD* f = FindStructField(field, &sdef);
				if (!f) throw std::runtime_error("Error: Unknown field '" + field + "' at line " + std::to_string(Peek().line));

				if (sdef->total_size > 1)
				{
					Gen::EmitLine("imul rax, rax, " + std::to_string(sdef->total_size));
				}
				Gen::EmitLine("add rcx, rax");
				if (f->offset > 0)
				{
					Gen::EmitLine("add rcx, " + std::to_string(f->offset));
				}
				uint8_t read_size = f->size;
				if (CheckOp(':'))
				{
					Advance();
					read_size = (uint8_t)Peek().value;
					Advance();
				}
				EmitLoadMem("rax", read_size, "[rcx]");
				g_LastExprSize = read_size;
				return;
			}

			// Plain byte offset indexing without .field
			Gen::EmitLine("add rcx, rax");
			uint8_t read_size = 1;
			if (CheckOp(':'))
			{
				Advance();
				read_size = (uint8_t)Peek().value;
				Advance();
			}
			EmitLoadMem("rax", read_size, "[rcx]");
			g_LastExprSize = read_size;
			return;
		}

		// Field access: name . field
		if (CheckOp('.'))
		{
			EmitLoadBasePtr("rcx", name);
			Advance(); // skip .
			std::string field = Peek().name;
			Advance();
			const STRUCT_DEF* sdef = nullptr;
			const STRUCT_FIELD* f = FindStructField(field, &sdef);
			if (!f) throw std::runtime_error("Error: Unknown field '" + field + "' at line " + std::to_string(Peek().line));

			if (f->offset > 0)
			{
				Gen::EmitLine("add rcx, " + std::to_string(f->offset));
			}
			uint8_t read_size = f->size;
			if (CheckOp(':'))
			{
				Advance();
				read_size = (uint8_t)Peek().value;
				Advance();
			}
			EmitLoadMem("rax", read_size, "[rcx]");
			g_LastExprSize = read_size;
			return;
		}

		// Variable byte slice read: var:size (e.g. a:1, a:2)
		uint8_t read_size = 8;
		if (!g_InBracketDeref && CheckOp(':'))
		{
			Advance();
			read_size = (uint8_t)Peek().value;
			Advance();
			g_LastExprSize = read_size;
		}

		auto it = g_Locals.find(name);
		if (it != g_Locals.end())
		{
			auto& var = it->second;
			if (read_size == 8 && var.size < 8)
				g_LastExprSize = var.size;
			uint8_t eff_size = (read_size < var.size) ? read_size : var.size;
			EmitLoadMem("rax", eff_size, "[rbp" + std::to_string(var.rbp_offset) + "]");
		}
		else
		{
			// Global variable fallback
			Gen::EnsureGlobalVar(name);
			EmitLoadMem("rax", read_size, "[" + name + "]");
		}

		return;
	}

	throw std::runtime_error("Error: Unexpected token in expression at line " + std::to_string(Peek().line));
}

static void ParseUnary()
{
	if (CheckOp('~'))
	{
		Advance();
		ParseUnary();
		Gen::EmitLine("not rax");
		return;
	}
	if (CheckOp('!'))
	{
		Advance();
		ParseUnary();
		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("setz al");
		Gen::EmitLine("movzx eax, al");
		return;
	}
	ParsePrimary();
}

static void ParseMulDiv()
{
	ParseUnary();
	while (!AtEnd() && Peek().type == OPERATOR &&
		(Peek().op == '*' || Peek().op == '/' || Peek().op == '%'))
	{
		uint16_t op = Peek().op;
		Advance();
		Gen::EmitLine("push rax");
		ParseUnary();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		if (op == '*')      Gen::EmitLine("imul rax, r10");
		else if (op == '/') { Gen::EmitLine("cqo"); Gen::EmitLine("idiv r10"); }
		else                { Gen::EmitLine("cqo"); Gen::EmitLine("idiv r10"); Gen::EmitLine("mov rax, rdx"); }
	}
}

static void ParseAddSub()
{
	ParseMulDiv();
	while (!AtEnd() && Peek().type == OPERATOR &&
		(Peek().op == '+' || Peek().op == '-'))
	{
		uint16_t op = Peek().op;
		Advance();
		Gen::EmitLine("push rax");
		ParseMulDiv();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		if (op == '+') Gen::EmitLine("add rax, r10");
		else           Gen::EmitLine("sub rax, r10");
	}
}

static void ParseShift()
{
	ParseAddSub();
	while (!AtEnd() && Peek().type == OPERATOR &&
		(Peek().op == LEFT_SHIFT || Peek().op == RIGHT_SHIFT))
	{
		uint16_t op = Peek().op;
		Advance();
		Gen::EmitLine("push rax");
		ParseAddSub();
		Gen::EmitLine("mov rcx, rax");
		Gen::EmitLine("pop rax");
		if (op == LEFT_SHIFT) Gen::EmitLine("shl rax, cl");
		else                  Gen::EmitLine("shr rax, cl");
	}
}

static void ParseRelational()
{
	ParseShift();
	while (!AtEnd() && Peek().type == OPERATOR &&
		(Peek().op == '<' || Peek().op == '>' || Peek().op == LESS_EQUAL || Peek().op == GREATER_EQUAL))
	{
		// If '<' is the start of a <param> or <global> assignment on a new line, it is not a less-than operator
		if (Peek().op == '<' && IsAngleRef())
		{
			// Check if followed by '=' or ':'
			if (g_Pos + 3 < g_pTokens->size())
			{
				uint16_t next_op = (*g_pTokens)[g_Pos + 3].op;
				if (next_op == '=' || next_op == ':')
					break;
			}
		}

		uint16_t op = Peek().op;
		Advance();
		Gen::EmitLine("push rax");
		ParseShift();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		Gen::EmitLine("cmp rax, r10");
		std::string setcc;
		if (op == '<')            setcc = "setl";
		else if (op == '>')       setcc = "setg";
		else if (op == LESS_EQUAL)  setcc = "setle";
		else                      setcc = "setge";
		Gen::EmitLine(setcc + " al");
		Gen::EmitLine("movzx eax, al");
	}
}

static void ParseEquality()
{
	ParseRelational();
	while (!AtEnd() && Peek().type == OPERATOR &&
		(Peek().op == EQUAL || Peek().op == NOT_EQUAL))
	{
		uint16_t op = Peek().op;
		Advance();
		Gen::EmitLine("push rax");
		ParseRelational();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		Gen::EmitLine("cmp rax, r10");
		if (op == EQUAL) Gen::EmitLine("sete al");
		else             Gen::EmitLine("setne al");
		Gen::EmitLine("movzx eax, al");
	}
}

static void ParseBitAnd()
{
	ParseEquality();
	while (!AtEnd() && Peek().type == OPERATOR && Peek().op == '&')
	{
		Advance();
		Gen::EmitLine("push rax");
		ParseEquality();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		Gen::EmitLine("and rax, r10");
	}
}

static void ParseBitXor()
{
	ParseBitAnd();
	while (!AtEnd() && Peek().type == OPERATOR && Peek().op == '^')
	{
		Advance();
		Gen::EmitLine("push rax");
		ParseBitAnd();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		Gen::EmitLine("xor rax, r10");
	}
}

static void ParseBitOr()
{
	ParseBitXor();
	while (!AtEnd() && Peek().type == OPERATOR && Peek().op == '|')
	{
		Advance();
		Gen::EmitLine("push rax");
		ParseBitXor();
		Gen::EmitLine("mov r10, rax");
		Gen::EmitLine("pop rax");
		Gen::EmitLine("or rax, r10");
	}
}

static void ParseLogicalAnd()
{
	ParseBitOr();
	while (!AtEnd() && Peek().type == OPERATOR && Peek().op == LOGICAL_AND)
	{
		Advance();
		std::string skip = Gen::NewLabel("land_false");
		std::string end = Gen::NewLabel("land_end");
		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("jz " + skip);
		ParseBitOr();
		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("jz " + skip);
		Gen::EmitLine("mov eax, 1");
		Gen::EmitLine("jmp " + end);
		Gen::EmitLabel(skip);
		Gen::EmitLine("xor eax, eax");
		Gen::EmitLabel(end);
	}
}

static void ParseLogicalOr()
{
	ParseLogicalAnd();
	while (!AtEnd() && Peek().type == OPERATOR && Peek().op == LOGICAL_OR)
	{
		Advance();
		std::string skip = Gen::NewLabel("lor_true");
		std::string end = Gen::NewLabel("lor_end");
		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("jnz " + skip);
		ParseLogicalAnd();
		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("jnz " + skip);
		Gen::EmitLine("xor eax, eax");
		Gen::EmitLine("jmp " + end);
		Gen::EmitLabel(skip);
		Gen::EmitLine("mov eax, 1");
		Gen::EmitLabel(end);
	}
}

static void ParseExpression()
{
	ParseLogicalOr();
}

// ============================================================
// Collect .nested blocks within a function
// ============================================================
static void CollectNestedBlocks()
{
	while (!AtEnd() && Check(KEYWORD, ".nested"))
	{
		Advance();
		ExpectOp('{', "{");

		while (!AtEnd() && !CheckOp('}'))
		{
			if (Peek().type != IDENTIFIER)
				throw std::runtime_error("Error: Expected nested routine name at line " + std::to_string(Peek().line));

			std::string nested_name = Peek().name;
			Advance();
			ExpectOp('{', "{");

			NESTED_FUNC nf;
			nf.name = nested_name;
			nf.parent_func = g_CurrentFunc.name;
			nf.token_start = g_Pos;

			int depth = 1;
			while (!AtEnd() && depth > 0)
			{
				if (CheckOp('{')) depth++;
				if (CheckOp('}')) depth--;
				if (depth > 0) Advance();
			}
			nf.token_end = g_Pos;
			Advance(); // skip }

			g_AllNestedFuncs.push_back(nf);

			if (CheckOp(',')) Advance();
		}

		ExpectOp('}', "}");
	}
}

// Parse block: { stmts... }
static void ParseBlock()
{
	ExpectOp('{', "{");
	while (!AtEnd() && !CheckOp('}'))
	{
		ParseStatement();
	}
	ExpectOp('}', "}");
}

// .then(param) { body } chain execution pipeline
static void ParseThenChain()
{
	while (!AtEnd() && Check(KEYWORD, ".then"))
	{
		Advance();
		ExpectOp('(', "(");
		std::string param_name = Peek().name;
		Advance();
		ExpectOp(')', ")");

		if (g_Locals.find(param_name) == g_Locals.end())
			AllocLocal(param_name, 8);
		auto& var = g_Locals[param_name];
		Gen::EmitLine("mov " + SizeDirective(var.size) + " [rbp" + std::to_string(var.rbp_offset) + "], " + RegForSize("rax", var.size));

		ParseBlock();
	}
}

// ============================================================
// Statement parsing
// ============================================================
static void ParseStatement()
{
	while (!AtEnd() && CheckOp(';')) Advance();
	if (AtEnd() || CheckOp('}')) return;

	// print(expr) — Output value to stdout like std::cout
	if (Check(KEYWORD, "print"))
	{
		Advance();
		ExpectOp('(', "(");
		ParseExpression(); // evaluate expression into rax
		ExpectOp(')', ")");
		Gen::EmitPrintRax();
		return;
	}

	// .nested block definition
	if (Check(KEYWORD, ".nested"))
	{
		CollectNestedBlocks();
		return;
	}

	// return [expr]
	if (Check(KEYWORD, "return"))
	{
		Advance();
		if (!CheckOp('}') && !CheckOp(';') && !AtEnd())
		{
			if (CheckOp('-'))
			{
				Advance();
				ParseExpression();
				Gen::EmitLine("neg rax");
			}
			else
			{
				ParseExpression();
			}
		}
		Gen::EmitLine("jmp " + g_ReturnLabel);
		return;
	}

	// break
	if (Check(KEYWORD, "break"))
	{
		Advance();
		if (g_LoopEndLabels.empty())
			throw std::runtime_error("Error: 'break' outside of loop at line " + std::to_string(Peek().line));
		Gen::EmitLine("jmp " + g_LoopEndLabels.top());
		return;
	}

	// loop { ... }
	if (Check(KEYWORD, "loop"))
	{
		Advance();
		std::string loop_start = Gen::NewLabel("loop");
		std::string loop_end = Gen::NewLabel("loop_end");
		g_LoopEndLabels.push(loop_end);
		Gen::EmitLabel(loop_start);
		ParseBlock();
		Gen::EmitLine("jmp " + loop_start);
		Gen::EmitLabel(loop_end);
		g_LoopEndLabels.pop();
		return;
	}

	// if (cond) { body } [else { body }]
	if (Check(KEYWORD, "if"))
	{
		Advance();
		ExpectOp('(', "(");
		ParseExpression();
		ExpectOp(')', ")");

		std::string else_label = Gen::NewLabel("else");
		std::string end_label = Gen::NewLabel("endif");

		Gen::EmitLine("test rax, rax");
		Gen::EmitLine("jz " + else_label);
		ParseBlock();

		if (Check(KEYWORD, "else"))
		{
			Advance();
			Gen::EmitLine("jmp " + end_label);
			Gen::EmitLabel(else_label);
			ParseBlock();
			Gen::EmitLabel(end_label);
		}
		else
		{
			Gen::EmitLabel(else_label);
		}
		return;
	}

	// nested routine call: nested name;
	if (Check(KEYWORD, "nested"))
	{
		Advance();
		std::string callee = Peek().name;
		Advance();
		if (CheckOp(';')) Advance();

		std::string label = g_CurrentFunc.name + "__nested__" + callee;
		Gen::EmitLine("call " + label);
		return;
	}

	// Angle bracket write: <a> = expr or <a>:size = expr
	// Semantic: Access & write to the original data of function parameter 'a'
	// Fallback: If not a local/parameter, writes to global variable 'a'
	if (IsAngleRef())
	{
		std::string name = ParseAngleRefName();
		uint8_t size = 8;
		if (CheckOp(':'))
		{
			Advance();
			size = (uint8_t)Peek().value;
			Advance();
		}
		ExpectOp('=', "=");

		auto it_orig = g_Locals.find(name + "$orig");
		if (it_orig != g_Locals.end())
		{
			Gen::EmitComment("write to original data of parameter <" + name + ">");
			ParseExpression(); // result in rax
			Gen::EmitLine("mov r10, QWORD PTR [rbp" + std::to_string(it_orig->second.rbp_offset) + "]");
			Gen::EmitLine("mov " + SizeDirective(size) + " [r10], " + RegForSize("rax", size));
			return;
		}

		auto it = g_Locals.find(name);
		if (it != g_Locals.end())
		{
			Gen::EmitComment("write to pointer <" + name + ">");
			ParseExpression(); // result in rax
			Gen::EmitLine("mov r10, QWORD PTR [rbp" + std::to_string(it->second.rbp_offset) + "]");
			Gen::EmitLine("mov " + SizeDirective(size) + " [r10], " + RegForSize("rax", size));
			return;
		}
		else
		{
			Gen::EnsureGlobalVar(name);
			ParseExpression(); // result in rax
			Gen::EmitLine("lea rcx, [" + name + "]");
			Gen::EmitLine("mov " + SizeDirective(size) + " [rcx], " + RegForSize("rax", size));
		}
		return;
	}

	// Memory dereference statement: [...] = expr or [expr:size]@offset = expr
	if (CheckOp('['))
	{
		ParsePrimary();
		if (Check(KEYWORD, ".then")) ParseThenChain();
		return;
	}

	// Identifier-led statement
	if (Peek().type == IDENTIFIER)
	{
		std::string name = Peek().name;
		size_t saved_pos = g_Pos;
		Advance();

		// Struct field write: name.field = expr or name.field:size = expr
		if (CheckOp('.'))
		{
			Advance(); // skip .
			std::string field = Peek().name;
			Advance();
			const STRUCT_DEF* sdef = nullptr;
			const STRUCT_FIELD* f = FindStructField(field, &sdef);
			if (f)
			{
				uint8_t write_size = f->size;
				if (CheckOp(':'))
				{
					Advance();
					write_size = (uint8_t)Peek().value;
					Advance();
				}
				if (CheckOp('='))
				{
					Advance();
					EmitLoadBasePtr("rcx", name);
					if (f->offset > 0)
					{
						Gen::EmitLine("add rcx, " + std::to_string(f->offset));
					}
					Gen::EmitLine("push rcx");
					g_LastExprSize = 0;
					ParseExpression();
					Gen::EmitLine("pop rcx");
					if (g_LastExprSize > 0 && g_LastExprSize < write_size)
						write_size = g_LastExprSize;
					Gen::EmitLine("mov " + SizeDirective(write_size) + " [rcx], " + RegForSize("rax", write_size));
					return;
				}
			}
			g_Pos = saved_pos;
		}

		// Struct field write with @ or indexed write: name@i.field = expr or name@i = expr
		if (CheckOp('@'))
		{
			Advance(); // skip @
			EmitLoadBasePtr("rcx", name);
			Gen::EmitLine("push rcx");
			ParseExpression(); // index in rax
			Gen::EmitLine("pop rcx");

			if (CheckOp('.'))
			{
				Advance(); // skip .
				std::string field = Peek().name;
				Advance();
				const STRUCT_DEF* sdef = nullptr;
				const STRUCT_FIELD* f = FindStructField(field, &sdef);
				if (!f) throw std::runtime_error("Error: Unknown field '" + field + "' at line " + std::to_string(Peek().line));

				if (sdef->total_size > 1)
				{
					Gen::EmitLine("imul rax, rax, " + std::to_string(sdef->total_size));
				}
				Gen::EmitLine("add rcx, rax");
				if (f->offset > 0)
				{
					Gen::EmitLine("add rcx, " + std::to_string(f->offset));
				}
				uint8_t write_size = f->size;
				if (CheckOp(':'))
				{
					Advance();
					write_size = (uint8_t)Peek().value;
					Advance();
				}
				if (CheckOp('='))
				{
					Advance();
					Gen::EmitLine("push rcx");
					g_LastExprSize = 0;
					ParseExpression();
					Gen::EmitLine("pop rcx");
					if (g_LastExprSize > 0 && g_LastExprSize < write_size)
						write_size = g_LastExprSize;
					Gen::EmitLine("mov " + SizeDirective(write_size) + " [rcx], " + RegForSize("rax", write_size));
					return;
				}
			}
			else
			{
				Gen::EmitLine("add rcx, rax");
				uint8_t write_size = 1;
				if (CheckOp(':'))
				{
					Advance();
					write_size = (uint8_t)Peek().value;
					Advance();
				}
				if (CheckOp('='))
				{
					Advance();
					Gen::EmitLine("push rcx");
					g_LastExprSize = 0;
					ParseExpression();
					Gen::EmitLine("pop rcx");
					if (g_LastExprSize > 0 && g_LastExprSize < write_size)
						write_size = g_LastExprSize;
					Gen::EmitLine("mov " + SizeDirective(write_size) + " [rcx], " + RegForSize("rax", write_size));
					return;
				}
			}
			g_Pos = saved_pos;
		}

		// If name is an existing global variable (not shadowed by a local variable)
		if (g_Locals.find(name) == g_Locals.end() && Gen::IsGlobalVar(name))
		{
			if (CheckOp(':'))
			{
				Advance();
				uint8_t slice_size = (uint8_t)Peek().value;
				Advance();
				ExpectOp('=', "=");
				g_LastExprSize = 0;
				ParseExpression();
				uint8_t write_size = slice_size;
				if (g_LastExprSize > 0 && g_LastExprSize < write_size)
					write_size = g_LastExprSize;
				Gen::EmitLine("lea rcx, [" + name + "]");
				Gen::EmitLine("mov " + SizeDirective(write_size) + " [rcx], " + RegForSize("rax", write_size));
				return;
			}
			if (CheckOp('='))
			{
				Advance();
				g_LastExprSize = 0;
				ParseExpression();
				Gen::EmitLine("lea rcx, [" + name + "]");
				Gen::EmitLine("mov QWORD PTR [rcx], rax");
				return;
			}
		}

		if (CheckOp(':'))
		{
			Advance();
			uint8_t slice_size = (uint8_t)Peek().value;
			Advance();
			ExpectOp('=', "=");

			auto it = g_Locals.find(name);
			if (it != g_Locals.end())
			{
				auto& var = it->second;
				g_LastExprSize = 0;
				ParseExpression(); // RHS in rax
				uint8_t write_size = slice_size;
				if (g_LastExprSize > 0 && g_LastExprSize < write_size)
					write_size = g_LastExprSize;
				Gen::EmitLine("mov " + SizeDirective(write_size) + " [rbp" + std::to_string(var.rbp_offset) + "], " + RegForSize("rax", write_size));
			}
			else
			{
				AllocLocal(name, slice_size);
				auto& var = g_Locals[name];
				g_LastExprSize = 0;
				ParseExpression();
				Gen::EmitLine("mov " + SizeDirective(var.size) + " [rbp" + std::to_string(var.rbp_offset) + "], " + RegForSize("rax", var.size));
			}
			return;
		}

		if (CheckOp('='))
		{
			Advance();
			if (g_Locals.find(name) == g_Locals.end())
				AllocLocal(name, 8);
			auto& var = g_Locals[name];
			g_LastExprSize = 0;
			ParseExpression();
			Gen::EmitLine("mov " + SizeDirective(var.size) + " [rbp" + std::to_string(var.rbp_offset) + "], " + RegForSize("rax", var.size));
			return;
		}

		// Compound assignment: += -= *= /= &= |= ^= <<= >>= %=
		if (!AtEnd() && Peek().type == OPERATOR &&
			(Peek().op == ADD_ASSIGN || Peek().op == SUB_ASSIGN ||
			 Peek().op == MUL_ASSIGN || Peek().op == DIV_ASSIGN ||
			 Peek().op == MOD_ASSIGN || Peek().op == AND_ASSIGN ||
			 Peek().op == OR_ASSIGN  || Peek().op == XOR_ASSIGN ||
			 Peek().op == LEFT_SHIFT_ASSIGN || Peek().op == RIGHT_SHIFT_ASSIGN))
		{
			uint16_t op = Peek().op;
			Advance();

			auto it = g_Locals.find(name);
			if (it == g_Locals.end())
				throw std::runtime_error("Error: Undefined variable '" + name + "'");
			auto& var = it->second;

			EmitLoadMem("r10", var.size, "[rbp" + std::to_string(var.rbp_offset) + "]");

			ParseExpression(); // RHS in rax

			switch (op)
			{
			case ADD_ASSIGN: Gen::EmitLine("add r10, rax"); break;
			case SUB_ASSIGN: Gen::EmitLine("sub r10, rax"); break;
			case MUL_ASSIGN: Gen::EmitLine("imul r10, rax"); break;
			case DIV_ASSIGN:
				Gen::EmitLine("xchg rax, r10");
				Gen::EmitLine("cqo");
				Gen::EmitLine("idiv r10");
				Gen::EmitLine("mov r10, rax");
				break;
			case MOD_ASSIGN:
				Gen::EmitLine("xchg rax, r10");
				Gen::EmitLine("cqo");
				Gen::EmitLine("idiv r10");
				Gen::EmitLine("mov r10, rdx");
				break;
			case AND_ASSIGN: Gen::EmitLine("and r10, rax"); break;
			case OR_ASSIGN:  Gen::EmitLine("or r10, rax"); break;
			case XOR_ASSIGN: Gen::EmitLine("xor r10, rax"); break;
			case LEFT_SHIFT_ASSIGN:
				Gen::EmitLine("mov rcx, rax");
				Gen::EmitLine("shl r10, cl");
				break;
			case RIGHT_SHIFT_ASSIGN:
				Gen::EmitLine("mov rcx, rax");
				Gen::EmitLine("shr r10, cl");
				break;
			}

			Gen::EmitLine("mov " + SizeDirective(var.size) + " [rbp" + std::to_string(var.rbp_offset) + "], " + RegForSize("r10", var.size));
			return;
		}

		// Array index or member access expression: name[...] / name@...
		if (CheckOp('[') || CheckOp('@'))
		{
			g_Pos = saved_pos;
			ParseExpression();
			if (Check(KEYWORD, ".then")) ParseThenChain();
			return;
		}

		// Bare expression statement
		g_Pos = saved_pos;
		ParseExpression();
		if (Check(KEYWORD, ".then")) ParseThenChain();
		return;
	}

	// Fallback expression statement
	ParseExpression();
	if (Check(KEYWORD, ".then")) ParseThenChain();
}

// ============================================================
// Emit nested functions within parent function scope
// ============================================================
static void EmitNestedBodies(const std::string& parent_func)
{
	for (auto& nf : g_AllNestedFuncs)
	{
		if (nf.parent_func != parent_func) continue;

		std::string label = parent_func + "__nested__" + nf.name;
		Gen::EmitLabel(label);
		Gen::EmitComment("nested routine: " + nf.name);

		size_t saved = g_Pos;
		g_Pos = nf.token_start;

		while (g_Pos < nf.token_end)
		{
			if (CheckOp('}')) break;
			ParseStatement();
		}

		Gen::EmitLine("ret");
		g_Pos = saved;
	}
}

// ============================================================
// Top-level construct parsers
// ============================================================
static void ParseGlobalDefinition()
{
	std::string name;
	if (IsAngleRef())
	{
		name = ParseAngleRefName();
	}
	else
	{
		name = Peek().name;
		Advance();
	}

	uint32_t size = 8;
	if (CheckOp(':'))
	{
		Advance();
		if (Peek().type == CONSTANT)
		{
			size = (uint32_t)Peek().value;
			Advance();
		}
	}

	ExpectOp('=', "=");

	uint64_t value = 0;
	if (Peek().type == CONSTANT)
	{
		value = Peek().value;
		Advance();
	}
	else if (CheckOp('-') && g_Pos + 1 < g_pTokens->size() && Peek(1).type == CONSTANT)
	{
		Advance();
		value = (uint64_t)(-(int64_t)Peek().value);
		Advance();
	}

	Gen::DefineGlobalVar(name, value, size);
}

static void ParseFunction()
{
	std::string func_name = Peek().name;
	Advance();

	if (func_name == "main")
		Gen::g_HasUserMain = true;

	g_LastFuncName = func_name;

	// Parse parameters: ( param1, param2, ... )
	ExpectOp('(', "(");
	std::vector<std::string> params;
	while (!CheckOp(')'))
	{
		if (Peek().type == IDENTIFIER)
		{
			params.push_back(Peek().name);
			Advance();
		}
		if (CheckOp(',')) Advance();
	}
	ExpectOp(')', ")");

	// Initialize function scope
	g_CurrentFunc = {};
	g_CurrentFunc.name = func_name;
	g_CurrentFunc.params = params;
	g_CurrentFunc.next_offset = 0;
	g_Locals.clear();

	g_ReturnLabel = Gen::NewLabel(func_name + "_ret");

	// Windows x64 integer parameter registers
	const std::string param_regs[] = { "rcx", "rdx", "r8", "r9" };

	// Allocate parameters as local variables on stack
	for (size_t i = 0; i < params.size(); i++)
		AllocLocal(params[i], 8);

	// Estimate total stack allocation needed
	size_t body_start = g_Pos;
	int var_count = 0;
	{
		size_t scan = body_start;
		if (scan < g_pTokens->size() && (*g_pTokens)[scan].type == OPERATOR && (*g_pTokens)[scan].op == '{')
		{
			int depth = 1;
			scan++;
			while (scan < g_pTokens->size() && depth > 0)
			{
				auto& t = (*g_pTokens)[scan];
				if (t.type == OPERATOR && t.op == '{') depth++;
				if (t.type == OPERATOR && t.op == '}') depth--;
				if (t.type == IDENTIFIER && scan + 1 < g_pTokens->size())
				{
					auto& next = (*g_pTokens)[scan + 1];
					if (next.type == OPERATOR && (next.op == ':' || next.op == '='))
						var_count++;
				}
				if (t.type == KEYWORD && t.name == ".then")
					var_count++;
				scan++;
			}
		}
	}

	int stack_alloc = (int)(params.size() * 16) + (var_count * 8) + 64 + 48; // copy + orig_ptr + locals + scratch + shadow space
	if (stack_alloc % 16 != 0)
		stack_alloc += 16 - (stack_alloc % 16);

	// Reset locals for clean generation
	g_CurrentFunc.next_offset = 0;
	g_Locals.clear();
	for (size_t i = 0; i < params.size(); i++)
	{
		AllocLocal(params[i], 8);          // local copy
		AllocLocal(params[i] + "$orig", 8); // pointer to caller's original data
	}

	// Emit procedure declaration & prologue (Windows x64 ABI)
	Gen::EmitComment("============================================================");
	Gen::EmitComment("Function: " + func_name);
	Gen::EmitComment("============================================================");
	Gen::g_TextSection << "PUBLIC " << func_name << "\n";
	Gen::g_TextSection << func_name << " PROC\n";
	Gen::EmitLine("push rbp");
	Gen::EmitLine("mov rbp, rsp");
	Gen::EmitLine("sub rsp, " + std::to_string(stack_alloc));

	// Save incoming parameter registers to home stack slots
	for (size_t i = 0; i < params.size() && i < 4; i++)
	{
		auto& var_copy = g_Locals[params[i]];
		auto& var_orig = g_Locals[params[i] + "$orig"];
		Gen::EmitComment("initialize parameter " + params[i] + ": pointer and local copy");
		Gen::EmitLine("mov QWORD PTR [rbp" + std::to_string(var_orig.rbp_offset) + "], " + param_regs[i]);
		Gen::EmitLine("mov r10, " + param_regs[i]);
		Gen::EmitLine("mov r11, QWORD PTR [r10]");
		Gen::EmitLine("mov QWORD PTR [rbp" + std::to_string(var_copy.rbp_offset) + "], r11");
	}

	// Parse main function body
	ParseBlock();

	// Function epilogue
	Gen::EmitLabel(g_ReturnLabel);
	Gen::EmitLine("mov rsp, rbp");
	Gen::EmitLine("pop rbp");
	Gen::EmitLine("ret");

	// Emit all nested routines declared within this function
	EmitNestedBodies(func_name);

	Gen::g_TextSection << func_name << " ENDP\n\n";
	g_CurrentFunc.name.clear();
}

// ============================================================
// Main entry point
// ============================================================
void Gear::Parser::Parse(const std::vector<TOKEN>& tokens)
{
	g_pTokens = &tokens;
	g_Pos = 0;
	g_AllNestedFuncs.clear();
	g_InBracketDeref = false;
	g_LastFuncName.clear();
	g_StructDefs.clear();
	Gen::Reset();

	while (!AtEnd())
	{
		if (CheckOp(';')) { Advance(); continue; }

		// Struct definition: struct name { field:size ... }
		if (Check(KEYWORD, "struct"))
		{
			ParseStructDefinition();
			continue;
		}

		// Top-level Global variable definition: NAME = value or NAME:size = value (No special symbol needed!)
		if (Peek().type == IDENTIFIER &&
			g_Pos + 1 < g_pTokens->size() &&
			(*g_pTokens)[g_Pos + 1].type == OPERATOR &&
			((*g_pTokens)[g_Pos + 1].op == '=' || (*g_pTokens)[g_Pos + 1].op == ':') &&
			(*g_pTokens)[g_Pos + 1].op != '(')
		{
			ParseGlobalDefinition();
			continue;
		}

		// Top-level Global variable definition with <> (backward compatibility)
		if (IsAngleRef())
		{
			ParseGlobalDefinition();
			continue;
		}

		// Function definition: identifier ( params... ) { ... }
		if (Peek().type == IDENTIFIER &&
			g_Pos + 1 < g_pTokens->size() &&
			(*g_pTokens)[g_Pos + 1].type == OPERATOR &&
			(*g_pTokens)[g_Pos + 1].op == '(')
		{
			ParseFunction();
			continue;
		}

		throw std::runtime_error("Error: Unexpected token '" + Peek().name + "' at top level, line " + std::to_string(Peek().line));
	}

	// Enforce explicit main() entry point
	if (!Gen::g_HasUserMain)
	{
		throw std::runtime_error("Error: Missing 'main' entry point function. Every program must explicitly define 'main()'.");
	}
}
