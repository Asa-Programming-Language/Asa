#pragma once

#define VERSION "0.1.0-prealpha"
#define COMPILER_PRINTOUT "Asa compiler\n" VERSION

extern int verbosity;  // Verbosity of 0 is silent (no output), 1 is quiet, 2 is default, 3 is verbose
extern int optimizationLevel;

enum CompilerFlags {
	Flags_None = 0,
	Flags_CompilerDebug = 1,
	Flags_RunTests = 2,
	Flags_Run = 4,
	Flags_Debug = 8,
	Flags_PrintAST = 16,
	Flags_Time = 32,
};

enum WarningFlags {
	W_None = 0,
	W_Conversion = 1 << 0,
	W_Attributes = 1 << 1,
	W_All = 0b111111111111111111111111111111,
};

extern CompilerFlags compilerFlags;
extern WarningFlags warningFlags;

inline CompilerFlags operator|(CompilerFlags a, CompilerFlags b)
{
	return static_cast<CompilerFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline CompilerFlags& operator|=(CompilerFlags& a, CompilerFlags b)
{
	a = a | b;
	return a;
}
inline bool operator==(CompilerFlags a, CompilerFlags b)
{
	return (static_cast<int>(a) & static_cast<int>(b)) > 0;
}

inline WarningFlags operator|(WarningFlags a, WarningFlags b)
{
	return static_cast<WarningFlags>(static_cast<int>(a) | static_cast<int>(b));
}
inline WarningFlags& operator|=(WarningFlags& a, WarningFlags b)
{
	a = a | b;
	return a;
}
inline bool operator==(WarningFlags a, WarningFlags b)
{
	return (static_cast<int>(a) & static_cast<int>(b)) > 0;
}
