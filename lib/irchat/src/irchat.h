// IrChat.h
//
// Copyright (c) 2020-2026 Kristofer Berggren
// co-authored by Andre Desgualdo Pereira (desgua, kana)
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.
//
// Modeled on duchat.h's shape

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unordered_set>

#include "protocol.h"

class IrChat : public Protocol
{
public:
  IrChat();
  virtual ~IrChat();

  static std::string GetName() { return "IRC"; }
  static std::string GetLibName() { return "libirchat"; }
  static std::string GetCreateFunc() { return "CreateIrChat"; }
  static std::string GetSetupMessage() { return ""; }

  std::string GetProfileId() const;
  std::string GetProfileDisplayName() const;

  bool HasFeature(ProtocolFeature p_ProtocolFeature) const;
  bool IsGroupChat(const std::string& p_ChatId) const;
  std::string GetSelfId() const;

  bool SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId);
  bool LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId);
  bool CloseProfile();

  bool Login();
  bool Logout();

  // outgoing request-queue worker thread (mirrors DuChat::Process)
  void Process();

  // incoming socket-reader thread (new vs. DuChat — IRC is push-based)
  void ConnectionLoop();

  void SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage);
  void SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler);

private:
  struct IrcMessage
  {
    std::string prefix;
    std::string command;
    std::vector<std::string> params;
  };

  // connection handling
  bool ConnectSocket();
  void DisconnectSocket();
  bool SendLine(const std::string& p_Line);
  void HandleLine(const std::string& p_Line);
  static IrcMessage ParseLine(const std::string& p_Line);
  static std::string PrefixToNick(const std::string& p_Prefix);

  // config (TODO: swap ad-hoc parsing below for nchat's real Config class
  // once its API is confirmed — this mirrors what SgChat's m_Config does)
  void InitConfig();

  // request handling
  void PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage);

  // thread-safe dispatch to the UI-facing handler (mirrors SgChat's
  // CallMessageHandler, since both the reader thread and the request
  // worker thread can produce notifications here)
  void CallMessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage);

  // chat/contact bookkeeping — IRC has no server-fetched chat list, so
  // chats/contacts are discovered lazily as JOIN/PRIVMSG events arrive
  void EnsureChat(const std::string& p_ChatId, bool p_IsGroup);
  void EnsureContact(const std::string& p_ChatId, const std::string& p_Name);

private:
  std::string m_ProfileId;
  std::function<void(std::shared_ptr<ServiceMessage>)> m_MessageHandler;
  std::mutex m_HandlerMutex;

  // outgoing request worker (same shape as DuChat)
  bool m_Running = false;
  std::thread m_Thread;
  std::deque<std::shared_ptr<RequestMessage>> m_RequestsQueue;
  std::mutex m_ProcessMutex;
  std::condition_variable m_ProcessCondVar;

  // incoming socket reader
  std::thread m_ReaderThread;

  // connection
  int m_Socket = -1;
  std::mutex m_SocketMutex;
  std::string m_Host;
  int m_Port;
  std::string m_Nick;
  std::string m_User;
  std::string m_Password;
  std::string m_RealName;
  std::vector<std::string> m_AutoJoinChannels;
  std::string m_ProfileDir;

  void ReadLoop();
  void DoAutoJoin();
  bool m_AutoJoinDone = false;

  mutable std::mutex m_ChatsMutex;
  std::map<std::string, bool> m_KnownChats;
  std::map<std::string, std::string> m_KnownNicks;
  std::map<std::string, std::unordered_set<std::string>> m_PendingNames;

  // cache
  static const int s_CacheDirVersion = 0;

  // irc info
  void PostSystemMessage(const std::string& p_Text);
  int m_SysMsgCounter = 0;
  std::string m_ProfileDisplayName;
};

extern "C" IrChat* CreateIrChat();
