// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.Collections.Generic;

public class DDSCityEditorTarget : TargetRules
{
	public DDSCityEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V4;
		ExtraModuleNames.Add("CitySample");
		ExtraModuleNames.Add("CitySampleEditor");
		ExtraModuleNames.Add("CitySampleAnimGraphRuntime");
		ExtraModuleNames.Add("DDSCity");

		if (Type == TargetType.Editor && Platform == UnrealTargetPlatform.Linux && LinuxPlatform.bEnableThreadSanitizer)
		{
			string[] TSanDisabledPlugins = 
			{
				"NeuralNetworkInference",
				"RemoteControl",
				"Text3D",
			};

			foreach (string PluginName in TSanDisabledPlugins)
			{
				DisablePlugins.Add(PluginName);
				EnablePlugins.Remove(PluginName);
			}
		}
	}
}
