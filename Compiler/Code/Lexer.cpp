#include "Compiler.h"
#include "Lexer.h"

std::vector<TOKEN> Gear::Lexer::g_Tokens;

void Gear::Lexer::Tokenize(const std::string& src)
{
	if (src.empty()) return;

	const size_t length = src.size();
	uint32_t line = 1;

	for (size_t i = 0; i < length; i++)
	{
		if (src[i] == '\n') line++;
		if (std::isspace((uint8_t)src[i])) continue;

		TOKEN token = {};
		token.line = line;

		// Handle .keyword and .member
		if (src[i] == '.' && i + 1 < length && std::isalpha((uint8_t)src[i + 1]))
		{
			i++;
			token.name = '.' + GetIdentifier(src, i);
			token.type = GetIdentifierType(token.name);
			if (token.type == KEYWORD)
			{
				Gear::Lexer::g_Tokens.push_back(token);
			}
			else
			{
				TOKEN temp_token = {};
				temp_token.op = '.';
				temp_token.type = OPERATOR;
				temp_token.line = line;
				Gear::Lexer::g_Tokens.push_back(temp_token);
				token.name.erase(0, 1);
				Gear::Lexer::g_Tokens.push_back(token);
			}
			continue;
		}

		// Identifiers and keywords
		if (std::isalpha((uint8_t)src[i]) || src[i] == '_')
		{
			token.name = GetIdentifier(src, i);
			token.type = GetIdentifierType(token.name);
			Gear::Lexer::g_Tokens.push_back(token);
			continue;
		}

		// Number constants (decimal and hex)
		if (std::isdigit((uint8_t)src[i]))
		{
			token.type = CONSTANT;
			token.value = GetConstant(src, i);
			Gear::Lexer::g_Tokens.push_back(token);
			continue;
		}

		uint16_t op = GetOperator(src, i);
		if (op != NONE)
		{
			if (op == COMMENT)
			{
				// Skip to end of line
				while (i < length && src[i] != '\n') i++;
				if (i < length && src[i] == '\n') line++;
				continue;
			}
			token.op = op;
			token.type = OPERATOR;
			Gear::Lexer::g_Tokens.push_back(token);
			continue;
		}

		throw std::runtime_error("Error : Unexpected character '" + std::string(1, src[i]) + "' in line " + std::to_string(line));
	}

	return;
}

uint16_t GetOperator(const std::string& op, uint64_t& i)
{
	const size_t length = op.size();

	switch (op[i])
	{
	case '~':  case '?':  case '(':  case ')':
	case '{':  case '}':  case '[':  case ']':
	case ';':  case ',':  case '.':  case ':':
	case '@':
		return op[i];
	case '+':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return ADD_ASSIGN;
		}
		else
		{
			return '+';
		}
		break;
	case '-':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return SUB_ASSIGN;
		}
		else if (i + 1 < length && op[i + 1] == '>')
		{
			i++;
			return OBJECT_ACCESS;
		}
		else
		{
			return '-';

		}
		break;
	case '*':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return MUL_ASSIGN;
		}
		else
		{
			return '*';
		}
		break;
	case '/':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return DIV_ASSIGN;
		}
		else if (i + 1 < length && op[i + 1] == '/')
		{
			i++;
			return COMMENT;
		}
		else
		{
			return '/';
		}
		break;
	case '^':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return XOR_ASSIGN;
		}
		else
		{
			return '^';
		}
		break;
	case '%':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return MOD_ASSIGN;
		}
		else
		{
			return '%';
		}
		break;
	case '=':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return EQUAL;
		}
		else
		{
			return '=';
		}
		break;
	case '<':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return LESS_EQUAL;
		}
		else if (i + 1 < length && op[i + 1] == '<')
		{
			i++;
			if (i + 1 < length && op[i + 1] == '=')
			{
				i++;
				return LEFT_SHIFT_ASSIGN;
			}
			else
			{
				return LEFT_SHIFT;
			}
		}
		else
		{
			return '<';
		}
		break;
	case '>':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return GREATER_EQUAL;
		}
		else if (i + 1 < length && op[i + 1] == '>')
		{
			i++;
			if (i + 1 < length && op[i + 1] == '=')
			{
				i++;
				return RIGHT_SHIFT_ASSIGN;
			}
			else
			{
				return RIGHT_SHIFT;
			}
		}
		else
		{
			return '>';
		}
		break;
	case '!':
		if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return NOT_EQUAL;
		}
		else
		{
			return '!';
		}
		break;
	case '&':
		if (i + 1 < length && op[i + 1] == '&')
		{
			i++;
			return LOGICAL_AND;
		}
		else if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return AND_ASSIGN;
		}
		else
		{
			return '&';
		}
		break;
	case '|':
		if (i + 1 < length && op[i + 1] == '|')
		{
			i++;
			return LOGICAL_OR;
		}
		else if (i + 1 < length && op[i + 1] == '=')
		{
			i++;
			return OR_ASSIGN;
		}
		else
		{
			return '|';
		}
		break;
	}
	return NONE;
}

uint64_t GetConstant(const std::string& num, uint64_t& i)
{
	const size_t length = num.size();
	std::string number;

	// Handle hex constants: 0x...
	if (num[i] == '0' && i + 1 < length && (num[i + 1] == 'x' || num[i + 1] == 'X'))
	{
		number += num[i]; i++;
		number += num[i]; i++;
		while (i < length && std::isxdigit((uint8_t)num[i]))
		{
			number += num[i];
			i++;
		}
		i--;
		return std::stoull(number, nullptr, 16);
	}

	// Decimal constants
	while (i < length && std::isdigit((uint8_t)num[i]))
	{
		if (i + 1 < length && (std::isalpha((uint8_t)num[i + 1]) && num[i + 1] != 'x' && num[i + 1] != 'X') || num[i + 1] == '_')
		{
			// Allow identifiers like rx_desc to not collide — only throw if truly mixed
			// Actually, check: if current is digit and next is alpha (not hex prefix), that's an error
			// But we need to be careful — this is only reached if we started parsing a number
			throw std::runtime_error("Error: Variable name cannot start with a digit");
		}
		number += num[i];
		i++;
	}
	i--;
	return std::stoull(number);
}

std::string GetIdentifier(const std::string& src, uint64_t& i)
{
	const size_t length = src.size();
	std::string identifier;
	while (i < length && (std::isalnum((uint8_t)src[i]) || src[i] == '_'))
	{
		identifier += src[i];
		i++;
	}
	i--;
	return identifier;
}

uint8_t GetIdentifierType(const std::string& identifier)
{
	if (
		identifier == "if" || identifier == "else" || identifier == "loop" || identifier == "return" ||
		identifier == "nested" || identifier == ".then" || identifier == ".nested" ||
		identifier == "break" || identifier == "print"
	)
	{
		return KEYWORD;
	}
	else
	{
		return IDENTIFIER;
	}
}
