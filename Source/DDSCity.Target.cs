// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System;

public class DDSCityTarget : TargetRules
{
	public DDSCityTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V4;
		ExtraModuleNames.Add("CitySample");
		ExtraModuleNames.Add("DDSCity");

		if (BuildEnvironment == TargetBuildEnvironment.Unique)
		{
			bUseLoggingInShipping = true;
		}
	}
}
