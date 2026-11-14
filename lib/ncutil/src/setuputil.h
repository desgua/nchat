// setuputil.h
//
// Copyright (c) 2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#pragma once

#include <string>

class SetupUtil
{
public:
  enum AuthMethod
  {
    AuthMethodQrImage = 1,
    AuthMethodQrTerminal = 2,
    AuthMethodCode = 3,
  };

  static bool GetConfigOrEnvFlag(const std::string& p_EnvName);
  static bool HasGui();
  static std::string GetQrPath();
  static int PromptAuthMethod(const std::string& p_CodeLabel, int p_QrCols, int p_QrRows);
  static bool IsCommandAvailable(const std::string& p_Cmd);
  static void ShowImage(const std::string& p_Path);
};
