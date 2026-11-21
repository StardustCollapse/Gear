#pragma once

#include <iostream>
#include <vector>

#include "Lexer.h"

namespace Gear
{
	namespace Lexer
	{
		extern std::vector<TOKEN> g_Tokens;
		
		void Tokenize(const std::string& src);
	}

	// Here handle .then
	namespace Parser
	{
		void Parse(const std::vector<TOKEN>& tokens);
	}

	namespace Optimizer
	{
	
	}

	namespace Generator
	{
		std::string GetOutput();
	}
}
