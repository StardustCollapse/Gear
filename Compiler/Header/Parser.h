#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include "Lexer.h"

// ============================================================
// Variable info — tracks local variables on the stack frame
// ============================================================
struct VAR_INFO
{
	std::string name;
	int32_t rbp_offset = 0;   // negative offset from RBP
	uint8_t size = 8;         // byte size (1, 2, 4, 8)
};

// ============================================================
// Nested function info — a nested block within a function
// ============================================================
struct NESTED_FUNC
{
	std::string name;
	std::string parent_func;       // owning function
	size_t token_start = 0;        // index of first token in body
	size_t token_end = 0;          // index past the closing brace
};

// ============================================================
// Function info — tracks a top-level function
// ============================================================
struct FUNC_INFO
{
	std::string name;
	std::vector<std::string> params;
	int32_t stack_size = 0;
	int32_t next_offset = 0;
};

// ============================================================
// Global constant info — <NAME> = value
// ============================================================
struct GLOBAL_CONST
{
	std::string name;
	uint64_t value = 0;
};

// ============================================================
// Global reference info — resolved at compile-time or as extern
// ============================================================
struct GLOBAL_REF
{
	bool is_const = false;
	uint64_t value = 0;
	std::string name;
};

// ============================================================
// Parser — walks tokens directly, calls Generator to emit asm
// ============================================================
namespace Gear
{
	namespace Parser
	{
		void Parse(const std::vector<TOKEN>& tokens);
	}
}
