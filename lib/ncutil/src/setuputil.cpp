// setuputil.cpp
//
// Copyright (c) 2026 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "setuputil.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include <sys/ioctl.h>
#include <unistd.h>

#include "appconfig.h"
#include "fileutil.h"
#include "log.h"
#include "strutil.h"

bool SetupUtil::GetConfigOrEnvFlag(const std::string& p_EnvName)
{
  const std::string configName = StrUtil::ToLower(p_EnvName);

  if (AppConfig::GetBool(configName)) return true;

  const char* envVal = getenv(p_EnvName.c_str());
  if (envVal != nullptr)
  {
    AppConfig::SetBool(configName, true);
    return true;
  }

  return false;
}

bool SetupUtil::HasGui()
{
#ifdef __APPLE__
  LOG_DEBUG("has gui 1 (macOS)");
  return true;
#elif defined(__linux__)
  if (!IsCommandAvailable("xdg-open"))
  {
    LOG_DEBUG("has gui 0 (no xdg-open)");
    return false;
  }

  if (IsCommandAvailable("xset"))
  {
    const std::string xsetCmd =
      IsCommandAvailable("timeout") ? "timeout 1s xset q" : "xset q";
    if (system((xsetCmd + " > /dev/null 2>&1").c_str()) == 0)
    {
      LOG_DEBUG("has gui 1 (%s ok)", xsetCmd.c_str());
      return true;
    }

    // x11 not reachable, gui may still be present on a wayland-only session
    bool hasWayland = (getenv("WAYLAND_DISPLAY") != nullptr);
    LOG_DEBUG("has gui %d (%s failed)", hasWayland, xsetCmd.c_str());
    return hasWayland;
  }

  // xset not available for probing, fall back to environment check
  bool hasDisplay = (getenv("DISPLAY") != nullptr) || (getenv("WAYLAND_DISPLAY") != nullptr);
  LOG_DEBUG("has gui %d (env)", hasDisplay);
  return hasDisplay;
#else
  LOG_DEBUG("has gui 0 (other)");
  return false;
#endif
}

std::string SetupUtil::GetQrPath()
{
  // shared by all protocols, as only one at a time holds the console for auth
  const std::string tempDir = FileUtil::GetTempDir();
  FileUtil::MkDir(tempDir); // not yet created during setup
  return tempDir + "/qr.png";
}

int SetupUtil::PromptAuthMethod(const std::string& p_CodeLabel, int p_QrCols, int p_QrRows)
{
  // flags and gui detection only select the default, the user makes the choice
  const bool hasCode = !p_CodeLabel.empty();
  int defaultMethod = AuthMethodQrTerminal;
  if (hasCode && GetConfigOrEnvFlag("USE_PAIRING_CODE"))
  {
    defaultMethod = AuthMethodCode;
  }
  else if (GetConfigOrEnvFlag("USE_QR_TERMINAL"))
  {
    defaultMethod = AuthMethodQrTerminal;
  }
  else if (!AppConfig::GetStr("qr_open_command").empty() || HasGui())
  {
    defaultMethod = AuthMethodQrImage;
  }

  if (!isatty(STDIN_FILENO))
  {
    LOG_INFO("auth method %d (default, stdin not a tty)", defaultMethod);
    return defaultMethod;
  }

  const bool isOutTty = isatty(STDOUT_FILENO);
  const std::string dimOn = isOutTty ? "\033[2m" : "";
  const std::string dimOff = isOutTty ? "\033[0m" : "";

  std::string qrPath = GetQrPath();
  const char* home = getenv("HOME");
  if ((home != nullptr) && (*home != '\0') && (qrPath.rfind(home, 0) == 0))
  {
    qrPath = "~" + qrPath.substr(strlen(home));
  }

  std::string terminalHint;
  struct winsize ws;
  const int needRows = p_QrRows + 1; // cursor line after the qr code
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) && (ws.ws_col > 0) && (ws.ws_row > 0) &&
      ((ws.ws_col < p_QrCols) || (ws.ws_row < needRows)))
  {
    terminalHint = "(needs " + std::to_string(p_QrCols) + "x" + std::to_string(needRows) +
      ", terminal is " + std::to_string(ws.ws_col) + "x" + std::to_string(ws.ws_row) + ")";
  }

  // hints start in the same column, two spaces after the longest label
  const std::string qrImageLabel = "QR code in image viewer";
  const std::string qrTerminalLabel = "QR code in terminal";
  const size_t hintCol = std::max(qrImageLabel.size(), qrTerminalLabel.size()) + 2;
  auto printOption = [&](int p_Num, const std::string& p_Label, const std::string& p_Hint)
  {
    std::cout << p_Num << ". " << p_Label;
    if (!p_Hint.empty())
    {
      std::cout << std::string(hintCol - p_Label.size(), ' ') << dimOn << p_Hint << dimOff;
    }
    std::cout << "\n";
  };

  std::cout << "\nAuthentication methods:\n";
  printOption(AuthMethodQrImage, qrImageLabel, "(" + qrPath + ")");
  printOption(AuthMethodQrTerminal, qrTerminalLabel, terminalHint);
  if (hasCode)
  {
    printOption(AuthMethodCode, p_CodeLabel, "");
  }

  const int maxMethod = hasCode ? AuthMethodCode : AuthMethodQrTerminal;
  int method = defaultMethod;
  while (true)
  {
    std::cout << "Select method (" << defaultMethod << "): " << std::flush;
    std::string choice;
    if (!std::getline(std::cin, choice))
    {
      std::cout << "\n";
      break;
    }

    StrUtil::Trim(choice);
    if (choice.empty()) break;

    if ((choice.size() == 1) && (choice[0] >= '1') && (choice[0] <= ('0' + maxMethod)))
    {
      method = choice[0] - '0';
      break;
    }

    std::cout << "Invalid choice.\n";
  }

  LOG_INFO("auth method %d (default %d)", method, defaultMethod);
  return method;
}

bool SetupUtil::IsCommandAvailable(const std::string& p_Cmd)
{
  const std::string cmd = "command -v " + p_Cmd + " > /dev/null 2>&1";
  return (system(cmd.c_str()) == 0);
}

void SetupUtil::ShowImage(const std::string& p_Path)
{
  std::string cmd = AppConfig::GetStr("qr_open_command");
  if (cmd.empty())
  {
#ifdef __APPLE__
    cmd = "open '%1' &";
#else
    cmd = "xdg-open >/dev/null 2>&1 '%1' &";
#endif
  }

  StrUtil::ReplaceString(cmd, "%1", StrUtil::EscapeSingleQuote(p_Path));

  int rv = system(cmd.c_str());
  if (rv != 0)
  {
    LOG_WARNING("show image '%s' failed %d", cmd.c_str(), rv);
  }
  else
  {
    LOG_DEBUG("show image '%s' ok", cmd.c_str());
  }
}
