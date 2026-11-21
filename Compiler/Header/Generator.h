#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>

// ============================================================
// Generator — emits x86-64 MASM assembly (Windows x64 ABI)
// ============================================================
namespace Gear
{
	namespace Generator
	{
		struct GLOBAL_VAR_INFO
		{
			uint64_t init_val = 0;
			uint32_t size = 8;
		};

		// Output buffers
		extern std::ostringstream g_DataSection;
		extern std::ostringstream g_TextSection;
		extern std::map<std::string, GLOBAL_VAR_INFO> g_GlobalVars;
		extern bool g_HasUserMain;
		extern bool g_NeedsPrint;

		// Unique label generator
		extern int g_LabelCounter;
		std::string NewLabel(const std::string& prefix);

		// Register / Define a global variable
		void DefineGlobalVar(const std::string& name, uint64_t init_val, uint32_t size = 8);
		void EnsureGlobalVar(const std::string& name);
		bool IsGlobalVar(const std::string& name);

		// Emit helpers
		void EmitLine(const std::string& line);
		void EmitLabel(const std::string& label);
		void EmitComment(const std::string& comment);

		// Print support (prints value in RAX to stdout)
		void EmitPrintRax();

		// Reset all buffers
		void Reset();

		// Get the final assembly output
		std::string GetOutput();
	}
}
