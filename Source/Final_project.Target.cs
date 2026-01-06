// Fill out your copyright notice in the Description page of Project Settings. [COPYRIGHT NOTICE PLACEHOLDER]

using UnrealBuildTool; // Import Unreal Build Tool namespace for target building [UBT IMPORT]
using System.Collections.Generic; // Import .NET collections for generic list operations [COLLECTIONS IMPORT]

public class Final_projectTarget : TargetRules // Define target rules class inheriting from TargetRules [TARGET RULES CLASS]
{ // Start of class body [CLASS BODY START]
	public Final_projectTarget(TargetInfo Target) : base(Target) // Constructor taking target info parameter and calling base constructor [CONSTRUCTOR]
	{ // Start of constructor body [CONSTRUCTOR BODY START]
		Type = TargetType.Game; // Set target type to Game for runtime executable [TARGET TYPE ASSIGNMENT]
		DefaultBuildSettings = BuildSettingsVersion.V5; // Use Unreal Engine 5 build settings version [BUILD SETTINGS VERSION]
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_6; // Set include order version for Unreal 5.6 compatibility [INCLUDE ORDER VERSION]
		CppStandard = CppStandardVersion.Cpp20; // Use C++20 language standard for modern features [CPP STANDARD VERSION]

		ExtraModuleNames.AddRange( new string[] { "Final_project" } ); // Add Final_project module to the list of modules to build [EXTRA MODULES ADDITION]
	} // End of constructor body [CONSTRUCTOR BODY END]
} // End of target rules class [TARGET RULES CLASS END]
