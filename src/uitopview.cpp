// uitopview.cpp
//
// Copyright (c) 2019-2024 Kristofer Berggren
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.

#include "uitopview.h"

#include "appconfig.h"
#include "apputil.h"
#include "status.h"
#include "strutil.h"
#include "uicolorconfig.h"
#include "uiconfig.h"
#include "uimodel.h"

UiTopView::UiTopView(const UiViewParams& p_Params)
  : UiViewBase(p_Params)
{
}

static std::string StatusToHumanStr(const std::string& p_StatusStr)
{
  static const std::map<std::string, std::string> statusStrMap =
  {
    { "Connecting", "󱒓" },
    { "Online", "󰱔" },
    { "Fetching", "󰇚" },
    { "Offline", "" },
    { "Sending", "󰕒" },
  };

  auto it = statusStrMap.find(p_StatusStr);
  return (it != statusStrMap.end()) ? it->second : p_StatusStr;
}

void UiTopView::Draw()
{
  static const bool awayStatusIndication = UiConfig::GetBool("away_status_indication");
  static const uint32_t fullMask = ~static_cast<uint32_t>(0);
  static const uint32_t statusMask = fullMask & (awayStatusIndication ? fullMask : ~Status::FlagAway);

  static uint32_t lastStatus = 0;
  uint32_t status = Status::Get(statusMask);

  static int lastUnreadCount = -1;
  int unreadCount = m_Model->GetUnreadCountLocked();

  std::pair<std::string, std::string>& currentChat = m_Model->GetCurrentChatLocked();

  std::string chatInfoStr;
  if (!(currentChat.first.empty() && currentChat.second.empty()))
  {
    std::string name = m_Model->GetContactListNameLocked(currentChat.first, currentChat.second,
                                                          true /*p_AllowId*/, true /*p_AllowAlias*/);
    if (!m_Model->GetEmojiEnabledLocked())
    {
      name = StrUtil::Textize(name);
    }

    std::string profileDisplayName = m_Model->GetProfileSuffixLocked(currentChat.first);
    std::string chatStatus = m_Model->GetChatStatusLocked(currentChat.first, currentChat.second);

    chatInfoStr = name + profileDisplayName + chatStatus;

    static const std::string phoneNumberIndicator = UiConfig::GetStr("phone_number_indicator");
    if (!phoneNumberIndicator.empty())
    {
      static std::string placeholder = "%1";
      static const bool isDynamicIndicator = (phoneNumberIndicator.find(placeholder) != std::string::npos);
      if (isDynamicIndicator)
      {
        std::string dynamicIndicator = phoneNumberIndicator;
        std::string phone = m_Model->GetContactPhoneLocked(currentChat.first, currentChat.second);
        StrUtil::ReplaceString(dynamicIndicator, placeholder, phone);
        chatInfoStr += " " + dynamicIndicator;
      }
      else
      {
        chatInfoStr += " " + phoneNumberIndicator;
      }
    }

    static const bool developerMode = AppUtil::GetDeveloperMode();
    if (developerMode)
    {
      chatInfoStr += " chat " + currentChat.second;
      int64_t lastMessageTime = m_Model->GetLastMessageTimeLocked(currentChat.first, currentChat.second);
      chatInfoStr += " time " + std::to_string(lastMessageTime);
      std::string phone = m_Model->GetContactPhoneLocked(currentChat.first, currentChat.second);
      if (!phone.empty())
      {
        chatInfoStr += " phone " + phone;
      }
    }
  }

  bool isListFocused = m_Model->IsListFocusedLocked();
  bool isHistoryFocused = m_Model->IsHistoryFocusedLocked();
  bool isEntryFocused = m_Model->IsEntryFocusedLocked();
  std::string focusName = m_Model->GetFocusedFrameNameLocked();

  static std::string lastChatInfoStr;
  static bool lastIsListFocused = false;
  static bool lastIsHistoryFocused = false;
  static bool lastIsEntryFocused = false;
  static std::string lastFocusName;

  m_Dirty |= (status != lastStatus) || (unreadCount != lastUnreadCount) || (chatInfoStr != lastChatInfoStr) ||
    (isListFocused != lastIsListFocused) || (isHistoryFocused != lastIsHistoryFocused) ||
    (isEntryFocused != lastIsEntryFocused) || (focusName != lastFocusName);

  lastStatus = status;
  lastUnreadCount = unreadCount;
  lastChatInfoStr = chatInfoStr;
  lastIsListFocused = isListFocused;
  lastIsHistoryFocused = isHistoryFocused;
  lastIsEntryFocused = isEntryFocused;
  lastFocusName = focusName;

  if (!m_Dirty) return;
  m_Dirty = false;

  m_Model->OnStatusUpdateLocked(status);

  if (!m_Enabled) return;

  curs_set(0);

  int topPadLeft = 1;
  int topPadRight = 1;
  static int colorPair = UiColorConfig::GetColorPair("top_color");
  static int attribute = UiColorConfig::GetAttribute("top_attr");
  static int colorPairList = UiColorConfig::GetColorPair("vim_navigation_status_color_list");
  static int colorPairHistory = UiColorConfig::GetColorPair("vim_navigation_status_color_history");

  int focusColorPair = colorPair;
  if (isListFocused)
  {
    focusColorPair = colorPairList;
  }
  else if (isHistoryFocused)
  {
    focusColorPair = colorPairHistory;
  }
  else if (isEntryFocused)
  {
    focusColorPair = colorPair;
  }

  werase(m_Win);
  wbkgd(m_Win, attribute | focusColorPair | ' ');
  wattron(m_Win, attribute | focusColorPair);

  // simple proxy config check
  static const std::string statusSuffixStr = []()
  {
    const std::string proxyHost = AppConfig::GetStr("proxy_host");
    const int proxyPort = AppConfig::GetNum("proxy_port");
    const bool proxyEnabled = (!proxyHost.empty() && (proxyPort != 0));
    if (proxyEnabled)
    {
      std::string proxyIndicator = UiConfig::GetStr("proxy_indicator");
      return " " + proxyIndicator;
    }

    return std::string("");
  }();

  const std::string unreadStr = (unreadCount > 0) ? ("[" + std::to_string(unreadCount) + "] ") : "";
  const std::string statusStr = StatusToHumanStr(Status::ToString(status)) + statusSuffixStr;

  std::wstring topWStrLeft = StrUtil::ToWString(unreadStr);
  std::wstring topWStrMid = L"   " + StrUtil::ToWString(chatInfoStr);
  std::wstring topWStrRight = L"   " + StrUtil::ToWString(statusStr + std::string(topPadRight, ' '));

  std::wstring leftPrefix;
  if (!focusName.empty())
  {
    leftPrefix = std::wstring(topPadLeft, ' ') + StrUtil::ToWString(focusName) + L" ";
  }
  else
  {
    leftPrefix = std::wstring(topPadLeft, ' ');
  }

  int leftPrefixWidth = StrUtil::WStringWidth(leftPrefix);
  int topStrLeftWidth = StrUtil::WStringWidth(topWStrLeft);
  int topStrRightWidth = StrUtil::WStringWidth(topWStrRight);
  int topStrMidWidth = StrUtil::WStringWidth(topWStrMid);

  int availableMidWidth = m_W - leftPrefixWidth - topStrLeftWidth - topStrRightWidth;
  if (availableMidWidth < 0)
  {
    availableMidWidth = 0;
  }

  if (topStrMidWidth > availableMidWidth)
  {
    topWStrMid = topWStrMid.substr(0, availableMidWidth);
    topStrMidWidth = StrUtil::WStringWidth(topWStrMid);
  }

  int leftGap = (availableMidWidth - topStrMidWidth) / 2;
  int rightGap = availableMidWidth - topStrMidWidth - leftGap;

  std::wstring topWStr = leftPrefix + topWStrLeft + std::wstring(leftGap, ' ') + topWStrMid +
    std::wstring(rightGap, ' ') + topWStrRight;

  topWStr = StrUtil::TrimPadWString(topWStr, m_W);
  mvwaddnwstr(m_Win, 0, 0, topWStr.c_str(), topWStr.size());

  wattroff(m_Win, attribute | focusColorPair);
  wrefresh(m_Win);
}
