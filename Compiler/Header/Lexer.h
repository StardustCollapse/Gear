#pragma once

#include <string>

typedef enum
{
	IDENTIFIER,
	KEYWORD,
	CONSTANT,
	OPERATOR,
	NONE_TYPE
} TOKEN_TYPE;

typedef enum
{
	NONE = 256,
	ADD_ASSIGN,
	SUB_ASSIGN,
	MUL_ASSIGN,
	DIV_ASSIGN,
	MOD_ASSIGN,
	XOR_ASSIGN,
	AND_ASSIGN,
	OR_ASSIGN,
	NOT_EQUAL,
	EQUAL,
	GREATER_EQUAL,
	LESS_EQUAL,
	LEFT_SHIFT,
	RIGHT_SHIFT,
	LEFT_SHIFT_ASSIGN,
	RIGHT_SHIFT_ASSIGN,
	LOGICAL_AND,
	LOGICAL_OR,
	OBJECT_ACCESS,
	COMMENT
} TOKEN_OPERATOR;

struct _TOKEN
{
	std::string name;
	uint64_t value = 0;
	uint32_t line = 0;
	uint16_t op = NONE;
	uint8_t type = NONE_TYPE;
};

typedef struct _TOKEN TOKEN, *PTOKEN;

uint16_t GetOperator(const std::string& op, uint64_t& i);

uint64_t GetConstant(const std::string& constant, uint64_t& i);

std::string GetIdentifier(const std::string& src, uint64_t& i);

uint8_t GetIdentifierType(const std::string& identifier);
