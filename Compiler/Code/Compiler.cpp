#define _CRT_SECURE_NO_WARNINGS
#include "Compiler.h"
#include <fstream>
#include <cstdlib>
#include <filesystem>
#include <array>

namespace fs = std::filesystem;

static std::string FindVcvars64()
{
	if (std::system("where ml64 > nul 2>&1") == 0)
		return "";

	const char* vc_dir = std::getenv("VCINSTALLDIR");
	if (vc_dir)
	{
		fs::path p = fs::path(vc_dir) / "Auxiliary" / "Build" / "vcvars64.bat";
		if (fs::exists(p)) return p.string();
	}

	std::string vswhere_path = "C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
	if (fs::exists(vswhere_path))
	{
		std::string cmd = "\"" + vswhere_path + "\" -latest -property installationPath";
		std::array<char, 256> buffer;
		std::string result;
		FILE* pipe = _popen(cmd.c_str(), "r");
		if (pipe)
		{
			while (fgets(buffer.data(), (int)buffer.size(), pipe) != nullptr)
				result += buffer.data();
			_pclose(pipe);
			while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
				result.pop_back();
			if (!result.empty())
			{
				fs::path p = fs::path(result) / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
				if (fs::exists(p)) return p.string();
			}
		}
	}

	const char* defaults[] = {
		"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
		"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Auxiliary\\Build\\vcvars64.bat",
		"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\VC\\Auxiliary\\Build\\vcvars64.bat",
		"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\VC\\Auxiliary\\Build\\vcvars64.bat",
		"C:\\Program Files (x86)\\Microsoft Visual Studio\\2019\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat",
	};
	for (const char* p : defaults)
	{
		if (fs::exists(p)) return p;
	}

	return "";
}

int main(int argc, char* argv[])
{
	if (argc < 2)
	{
		std::cout << "Usage : Compiler <filename> [--no-run]" << std::endl;
		return 1;
	}

	std::ifstream file(argv[1]);
	if (!file.is_open())
	{
		std::cerr << "Error opening file: " << argv[1] << std::endl;
		return 1;
	}

	std::string source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	file.close();

	try
	{
		// Phase 1: Lexer (Tokenizing)
		Gear::Lexer::Tokenize(source);
		std::cout << "[Gear Lexer] " << Gear::Lexer::g_Tokens.size() << " tokens parsed." << std::endl;

		// Phase 2: Parser + Generator (Direct Token-Walking Code Generation)
		Gear::Parser::Parse(Gear::Lexer::g_Tokens);

		// Phase 3: Assembly File Output
		std::string asm_output = Gear::Generator::GetOutput();

		fs::path input_file(argv[1]);
		std::string stem = input_file.stem().string();

		fs::path root_dir = ".";
		if (!fs::exists("Gear.sln") && fs::exists("../Gear.sln"))
		{
			root_dir = "..";
		}

		fs::path build_dir = root_dir / "Build";
		fs::path bin_dir = root_dir / "Bin";
		fs::create_directories(build_dir);
		fs::create_directories(bin_dir);

		std::string asm_file = (build_dir / (stem + ".asm")).string();
		std::string obj_file = (build_dir / (stem + ".obj")).string();
		std::string exe_file = (bin_dir / (stem + ".exe")).string();

		std::ofstream out(asm_file);
		if (!out.is_open())
		{
			std::cerr << "Error: Cannot create output file " << asm_file << std::endl;
			return 1;
		}
		out << asm_output;
		out.close();
		std::cout << "[Gear Generator] MASM output written to: " << asm_file << std::endl;

		// Phase 4: Instant Assembly, Linking & Execution
		bool auto_run = true;
		if (argc >= 3 && std::string(argv[2]) == "--no-run")
			auto_run = false;

		if (auto_run)
		{
			std::string vcvars = FindVcvars64();
			std::string prefix = "";
			if (!vcvars.empty())
			{
				prefix = "call \"" + vcvars + "\" > nul 2>&1 && ";
			}

			std::string asm_cmd = prefix + "ml64 /nologo /Fo\"" + obj_file + "\" /c \"" + asm_file + "\"";
			int asm_rc = std::system(asm_cmd.c_str());
			if (asm_rc != 0)
			{
				std::cerr << "[Gear Error] ml64 assembly failed (code " << asm_rc << "). Make sure MSVC x64 build tools are installed." << std::endl;
			}
			else
			{
				std::string link_cmd = prefix + "link /nologo /subsystem:console /defaultlib:msvcrt.lib /defaultlib:legacy_stdio_definitions.lib \"" + obj_file + "\" /out:\"" + exe_file + "\"";
				int link_rc = std::system(link_cmd.c_str());
				if (link_rc != 0)
				{
					std::cerr << "[Gear Error] link failed (code " << link_rc << ")." << std::endl;
				}
				else
				{
					std::cout << "\n================ [Gear Instant Run] ================\n";
					std::fflush(stdout);
					std::system(("\"" + exe_file + "\"").c_str());
					std::cout << "====================================================\n";
				}
			}
		}
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return 1;
	}

	return 0;
}
