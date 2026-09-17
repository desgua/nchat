// IrChat.cpp
//
// Copyright (c) 2020-2026 Kristofer Berggren
// co-authored by André Desgualdo Pereira (desgua, kana)
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.
//
// Deliberately NOT covered yet (left as TODOs / return no-ops):
//   - TLS (plain-text socket only for now)
//   - SASL auth
//   - PART/QUIT membership updates, NAMES (353/366) group member sync
//   - reactions, edit, pin, archive, delete, typing (no IRC equivalent)
//   - real Config class integration (using ad-hoc parsing for now)

#include "irchat.h"

#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "log.h"
#include "status.h"
#include "messagecache.h"

extern "C" IrChat* CreateIrChat()
{
  return new IrChat();
}

IrChat::IrChat()
{
  m_ProfileId = GetName();
}

IrChat::~IrChat()
{
}

std::string IrChat::GetProfileId() const
{
  return m_ProfileId;
}

std::string IrChat::GetProfileDisplayName() const
{
  static std::string profileDisplayName = "";
  return profileDisplayName;
}

bool IrChat::HasFeature(ProtocolFeature p_ProtocolFeature) const
{
  // No FeatureAutoGetChatsOnLogin: unlike WhatsApp/Signal (which get a
  // full chat list pushed from their own SDK's login flow), IRC has
  // nothing to auto-push — we need the UI to actively call
  // GetChatsRequestType so our MessageCache::FetchChats + in-memory
  // m_KnownChats logic actually runs.
  static int customFeatures = FeatureNone;
  return (p_ProtocolFeature & customFeatures);
}

bool IrChat::IsGroupChat(const std::string& p_ChatId) const
{
  // Standard IRC channel prefixes.
  return !p_ChatId.empty() && ((p_ChatId[0] == '#') || (p_ChatId[0] == '&'));
}

std::string IrChat::GetSelfId() const
{
  return m_Nick;
}

bool IrChat::SetupProfile(const std::string& p_ProfilesDir, std::string& p_ProfileId)
{
  m_ProfileId = m_ProfileId + "_" + m_Host;
  std::string profileDir = p_ProfilesDir + "/" + m_ProfileId;

  mkdir(profileDir.c_str(), 0777);

  MessageCache::AddProfile(m_ProfileId, false /*p_CheckSequence*/, s_CacheDirVersion, true /*p_IsSetup*/,
                         false /*p_AllowReadOnly*/);
  // TODO: interactively prompt for host/port/nick/channels here, the way
  // wmchat/sgchat prompt for phone number, and write them into a config
  // file under profileDir (see InitConfig for the read side).

  p_ProfileId = m_ProfileId;
  return true;
}

bool IrChat::LoadProfile(const std::string& p_ProfilesDir, const std::string& p_ProfileId)
{
  m_ProfileId = p_ProfileId;
  m_ProfileDir = p_ProfilesDir + "/" + p_ProfileId;
  MessageCache::AddProfile(m_ProfileId, false /*p_CheckSequence*/, s_CacheDirVersion, false /*p_IsSetup*/,
                           false /*p_AllowReadOnly*/);
  return true;
}

bool IrChat::CloseProfile()
{
  m_ProfileId = "";
  return true;
}

void IrChat::InitConfig()
{
  // Ad-hoc "key=value" line parser as a placeholder. Swap for nchat's
  // real Config class (see sgchat.h's m_Config) once its load/save API
  // is confirmed — this keeps the skeleton self-contained for now.
  std::ifstream file(m_ProfileDir + "/irc.conf");
  std::string line;
  while (std::getline(file, line))
  {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;

    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);

    if (key == "host") m_Host = val;
    else if (key == "port") m_Port = atoi(val.c_str());
    else if (key == "nick") m_Nick = val;
    else if (key == "user") m_User = val;
    else if (key == "password") m_Password = val;
    else if (key == "realname") m_RealName = val;
    else if (key == "channel") m_AutoJoinChannels.push_back(val);
  }
}

bool IrChat::ConnectSocket()
{
  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo* res = nullptr;
  std::string portStr = std::to_string(m_Port);
  int rc = getaddrinfo(m_Host.c_str(), portStr.c_str(), &hints, &res);
  if (rc != 0)
  {
    LOG_DEBUG("irc getaddrinfo failed for %s: %s", m_Host.c_str(), gai_strerror(rc));
    return false;
  }

  int sock = -1;
  for (struct addrinfo* p = res; p != nullptr; p = p->ai_next)
  {
    sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (sock == -1) continue;

    if (connect(sock, p->ai_addr, p->ai_addrlen) == 0)
    {
      break;
    }

    close(sock);
    sock = -1;
  }

  freeaddrinfo(res);

  if (sock == -1)
  {
    LOG_DEBUG("irc connect failed to %s:%d", m_Host.c_str(), m_Port);
    return false;
  }

  m_Socket = sock;
  LOG_DEBUG("irc connected to %s:%d", m_Host.c_str(), m_Port);
  return true;
}

void IrChat::DisconnectSocket()
{
  std::unique_lock<std::mutex> lock(m_SocketMutex);
  if (m_Socket != -1)
  {
    shutdown(m_Socket, SHUT_RDWR);
    close(m_Socket);
    m_Socket = -1;
  }
}

bool IrChat::SendLine(const std::string& p_Line)
{
  std::unique_lock<std::mutex> lock(m_SocketMutex);
  if (m_Socket == -1) return false;

  std::string wire = p_Line + "\r\n";
  size_t sent = 0;
  while (sent < wire.size())
  {
    ssize_t n = send(m_Socket, wire.c_str() + sent, wire.size() - sent, MSG_NOSIGNAL);
    if (n <= 0)
    {
      LOG_DEBUG("irc send failed");
      return false;
    }
    sent += n;
  }

  return true;
}

IrChat::IrcMessage IrChat::ParseLine(const std::string& p_Line)
{
  IrcMessage msg;
  std::string line = p_Line;
  if (!line.empty() && (line.back() == '\r'))
  {
    line.pop_back();
  }

  if (line.empty()) return msg;

  size_t pos = 0;
  if (line[0] == ':')
  {
    size_t sp = line.find(' ');
    if (sp == std::string::npos) return msg;
    msg.prefix = line.substr(1, sp - 1);
    pos = sp + 1;
  }

  size_t sp = line.find(' ', pos);
  if (sp == std::string::npos)
  {
    msg.command = line.substr(pos);
    return msg;
  }

  msg.command = line.substr(pos, sp - pos);
  pos = sp + 1;

  while (pos < line.size())
  {
    if (line[pos] == ':')
    {
      msg.params.push_back(line.substr(pos + 1));
      break;
    }

    size_t next = line.find(' ', pos);
    if (next == std::string::npos)
    {
      msg.params.push_back(line.substr(pos));
      break;
    }

    msg.params.push_back(line.substr(pos, next - pos));
    pos = next + 1;
  }

  return msg;
}

std::string IrChat::PrefixToNick(const std::string& p_Prefix)
{
  size_t bang = p_Prefix.find('!');
  return (bang == std::string::npos) ? p_Prefix : p_Prefix.substr(0, bang);
}

void IrChat::CallMessageHandler(std::shared_ptr<ServiceMessage> p_ServiceMessage)
{
  MessageCache::AddFromServiceMessage(m_ProfileId, p_ServiceMessage);

  std::unique_lock<std::mutex> lock(m_HandlerMutex);
  if (m_MessageHandler)
  {
    m_MessageHandler(p_ServiceMessage);
  }
}

void IrChat::EnsureChat(const std::string& p_ChatId, bool p_IsGroup)
{
  {
    std::unique_lock<std::mutex> lock(m_ChatsMutex);
    if (m_KnownChats.find(p_ChatId) != m_KnownChats.end()) return;
    m_KnownChats[p_ChatId] = p_IsGroup;
  }

  std::shared_ptr<NewChatsNotify> newChatsNotify = std::make_shared<NewChatsNotify>(m_ProfileId);
  newChatsNotify->success = true;
  ChatInfo chatInfo;
  chatInfo.id = p_ChatId;
  newChatsNotify->chatInfos.push_back(chatInfo);
  CallMessageHandler(newChatsNotify);
}

void IrChat::EnsureContact(const std::string& p_ChatId, const std::string& p_Name)
{
  {
    std::unique_lock<std::mutex> lock(m_ChatsMutex);
    auto it = m_KnownNicks.find(p_ChatId);
    if ((it != m_KnownNicks.end()) && (it->second == p_Name)) return;
    m_KnownNicks[p_ChatId] = p_Name;
  }

  std::shared_ptr<NewContactsNotify> newContactsNotify = std::make_shared<NewContactsNotify>(m_ProfileId);
  ContactInfo contactInfo;
  contactInfo.id = p_ChatId;
  contactInfo.name = p_Name;
  newContactsNotify->contactInfos.push_back(contactInfo);
  CallMessageHandler(newContactsNotify);
}

void IrChat::DoAutoJoin()
{
  {
    std::unique_lock<std::mutex> lock(m_ChatsMutex);
    if (m_AutoJoinDone) return;
    m_AutoJoinDone = true;
  }

  for (const std::string& channel : m_AutoJoinChannels)
  {
    SendLine("JOIN " + channel);
    LOG_DEBUG("irc request to join %s", channel.c_str());
  }
}

void IrChat::HandleLine(const std::string& p_Line)
{
  IrcMessage msg = ParseLine(p_Line);
  if (msg.command.empty()) return;

  LOG_DEBUG("irc recv: %s", p_Line.c_str());

  if (msg.command == "PING")
  {
    std::string token = msg.params.empty() ? "" : msg.params[0];
    SendLine("PONG :" + token);
    return;
  }

  if (msg.command == "001") // RPL_WELCOME — registration complete
  {
    Status::Set(m_ProfileId, Status::FlagOnline);
    Status::Clear(m_ProfileId, Status::FlagConnecting);

    std::shared_ptr<ConnectNotify> connectNotify = std::make_shared<ConnectNotify>(m_ProfileId);
    connectNotify->success = true;
    CallMessageHandler(connectNotify);

    LOG_DEBUG("irc registered as %s", m_Nick.c_str());

    if (!m_Password.empty())
    {
      std::string targetAccount = m_User.empty() ? m_Nick : m_User;
      SendLine("PRIVMSG NickServ :IDENTIFY " + targetAccount + " " + m_Password);
      LOG_DEBUG("irc sent identify command for account %s", targetAccount.c_str());
      // DoAutoJoin() is now triggered by "900" below, with a fallback timer
      // as a safety net for networks that don't send RPL_LOGGEDIN (900).
//      std::thread([this]()
//      {
//        std::this_thread::sleep_for(std::chrono::seconds(10));
//        LOG_DEBUG("irc request to join using fallback sleep timer %s");
//        DoAutoJoin();
//      }).detach();
    }
    else
    {
      DoAutoJoin();
    }
    return;

    for (const std::string& channel : m_AutoJoinChannels)
    {
      SendLine("JOIN " + channel);
      LOG_DEBUG("irc request to join %s", channel.c_str());
    }
    return;
  }

  if (msg.command == "433") // ERR_NICKNAMEINUSE
  {
    m_Nick += "_";
    LOG_DEBUG("irc nick in use, retrying as %s", m_Nick.c_str());
    SendLine("NICK " + m_Nick);
    return;
  }

  if (msg.command == "900") // RPL_LOGGEDIN — NickServ identify confirmed
  {
    LOG_DEBUG("irc identified successfully");
    DoAutoJoin();
    return;
  }

  if (msg.command == "JOIN")
  {
    if (msg.params.empty()) return;
    std::string channel = msg.params[0];
    std::string nick = PrefixToNick(msg.prefix);

    if (nick == m_Nick)
    {
      LOG_DEBUG("irc successfully joined %s", channel.c_str());
      EnsureChat(channel, true);
    }
    else
    {
      // TODO: track per-channel membership, push NewGroupMembersNotify.
      EnsureContact(nick, nick);
    }
    return;
  }

  if (msg.command == "PRIVMSG")
  {
    if (msg.params.size() < 2) return;

    std::string target = msg.params[0];
    std::string text = msg.params[1];
    std::string nick = PrefixToNick(msg.prefix);

    bool isGroup = IsGroupChat(target);
    std::string chatId = isGroup ? target : nick;

    EnsureChat(chatId, isGroup);
    EnsureContact(nick, nick);

    ChatMessage chatMessage;
    chatMessage.id = chatId + "_" + std::to_string(time(nullptr)) + "_" + nick;
    chatMessage.senderId = nick;
    chatMessage.text = text;
    chatMessage.timeSent = static_cast<int64_t>(time(nullptr)) * 1000;
    chatMessage.isOutgoing = false;
    chatMessage.isRead = false;

    std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
    newMessagesNotify->success = true;
    newMessagesNotify->chatId = chatId;
    newMessagesNotify->cached = false;
    newMessagesNotify->sequence = true;
    newMessagesNotify->chatMessages.push_back(chatMessage);
    CallMessageHandler(newMessagesNotify);
    return;
  }

  if ((msg.command == "473") || (msg.command == "474") || (msg.command == "475") ||
      (msg.command == "477") || (msg.command == "471") || (msg.command == "403"))
  {
    std::string detail = msg.params.empty() ? "" : msg.params.back();
    LOG_DEBUG("irc join failed (%s): %s", msg.command.c_str(), detail.c_str());
    return;
  }
  // TODO: PART, QUIT, NICK, NOTICE, 353/366 (NAMES), MODE, TOPIC, ...
  LOG_DEBUG("irc unhandled: %s", msg.command.c_str());
}

bool IrChat::Login()
{
  InitConfig();

  m_Running = true;
  m_Thread = std::thread(&IrChat::Process, this);
  m_ReaderThread = std::thread(&IrChat::ConnectionLoop, this);

  // Connecting now happens asynchronously inside ConnectionLoop (with
  // retry/backoff), so Login() no longer blocks on or reflects the
  // outcome of the first connection attempt — this is a behavior
  // change from before, where a failed initial connect made Login()
  // return false immediately.
  return true;
}

bool IrChat::Logout()
{
  m_Running = false; // tells ConnectionLoop not to reconnect after this

  if (m_Socket != -1)
  {
    SendLine("QUIT :nchat closing");
  }

  Status::Clear(m_ProfileId, Status::FlagOnline);
  Status::Clear(m_ProfileId, Status::FlagConnecting);

  {
    std::unique_lock<std::mutex> lock(m_ProcessMutex);
    m_ProcessCondVar.notify_one();
  }

  // Unblocks a blocking recv() in ReadLoop. Does NOT unblock an
  // in-progress blocking connect() during a retry attempt — a TODO,
  // would need a non-blocking connect + select/poll with timeout to
  // fix properly.
  DisconnectSocket();

  if (m_Thread.joinable())
  {
    m_Thread.join();
  }

  if (m_ReaderThread.joinable())
  {
    m_ReaderThread.join();
  }

  return true;
}

void IrChat::ConnectionLoop()
{
  int backoffSec = 1;
  const int maxBackoffSec = 60;

  while (m_Running)
  {
    Status::Set(m_ProfileId, Status::FlagConnecting);

    {
      std::unique_lock<std::mutex> lock(m_ChatsMutex);
      m_AutoJoinDone = false;
    }
    if (!ConnectSocket())
    {
      Status::Clear(m_ProfileId, Status::FlagConnecting);
      if (!m_Running) break;

      LOG_DEBUG("irc connect failed, retrying in %ds", backoffSec);
      for (int waited = 0; (waited < backoffSec) && m_Running; ++waited)
      {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
      backoffSec = std::min(backoffSec * 2, maxBackoffSec);
      continue;
    }

    backoffSec = 1; // reset backoff after any successful connect

    SendLine("NICK " + m_Nick);
    SendLine("USER " + m_User + " 0 * :" + m_RealName);

    ReadLoop(); // blocks until disconnected or m_Running goes false

    Status::Clear(m_ProfileId, Status::FlagOnline);
    DisconnectSocket();

    if (!m_Running) break;

    LOG_DEBUG("irc disconnected, reconnecting in %ds", backoffSec);
    Status::Set(m_ProfileId, Status::FlagConnecting);
    for (int waited = 0; (waited < backoffSec) && m_Running; ++waited)
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    backoffSec = std::min(backoffSec * 2, maxBackoffSec);

    // m_KnownChats/m_KnownNicks are intentionally NOT cleared here —
    // the UI already has those chats; re-registering triggers fresh
    // JOINs (below, via "001") which re-establishes membership without
    // needing duplicate NewChatsNotify calls for chats it already knows.
  }

  Status::Clear(m_ProfileId, Status::FlagConnecting);
}

void IrChat::ReadLoop()
{
  std::string buffer;
  char recvBuf[4096];

  while (m_Running)
  {
    ssize_t n = recv(m_Socket, recvBuf, sizeof(recvBuf), 0);
    if (n <= 0)
    {
      LOG_DEBUG("irc connection closed or errored");
      return;
    }

    buffer.append(recvBuf, static_cast<size_t>(n));

    size_t pos;
    while ((pos = buffer.find('\n')) != std::string::npos)
    {
      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);
      HandleLine(line);
    }
  }
}
void IrChat::Process()
{
  while (m_Running)
  {
    std::shared_ptr<RequestMessage> requestMessage;

    {
      std::unique_lock<std::mutex> lock(m_ProcessMutex);
      while (m_RequestsQueue.empty() && m_Running)
      {
        m_ProcessCondVar.wait(lock);
      }

      if (!m_Running) break;

      requestMessage = m_RequestsQueue.front();
      m_RequestsQueue.pop_front();
    }

    PerformRequest(requestMessage);
  }
}

void IrChat::SendRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  std::unique_lock<std::mutex> lock(m_ProcessMutex);
  m_RequestsQueue.push_back(p_RequestMessage);
  m_ProcessCondVar.notify_one();
}

void IrChat::SetMessageHandler(const std::function<void(std::shared_ptr<ServiceMessage>)>& p_MessageHandler)
{
  std::unique_lock<std::mutex> lock(m_HandlerMutex);
  m_MessageHandler = p_MessageHandler;
}

void IrChat::PerformRequest(std::shared_ptr<RequestMessage> p_RequestMessage)
{
  switch (p_RequestMessage->GetMessageType())
  {
    case GetChatsRequestType:
    {
      std::shared_ptr<GetChatsRequest> getChatsRequest =
        std::static_pointer_cast<GetChatsRequest>(p_RequestMessage);

      // Cached chats (channels rejoined via config AND private chats from
      // prior sessions, which have no other way to be rediscovered since
      // IRC servers don't remember them) — this pushes its own
      // NewChatsNotify asynchronously via our message handler.
      //MessageCache::FetchChats(m_ProfileId, getChatsRequest->chatIds);

      bool fetched = MessageCache::FetchChats(m_ProfileId, getChatsRequest->chatIds);
      LOG_DEBUG("irc cache fetch chats returned %d", fetched);

      // What we already know about in this running session.
      std::shared_ptr<NewChatsNotify> newChatsNotify = std::make_shared<NewChatsNotify>(m_ProfileId);
      newChatsNotify->success = true;

      std::unique_lock<std::mutex> lock(m_ChatsMutex);
      for (const auto& chat : m_KnownChats)
      {
        ChatInfo chatInfo;
        chatInfo.id = chat.first;
        newChatsNotify->chatInfos.push_back(chatInfo);
      }
      lock.unlock();

      CallMessageHandler(newChatsNotify);
    }
    break;

    case SendMessageRequestType:
      {
        std::shared_ptr<SendMessageRequest> sendMessageRequest =
          std::static_pointer_cast<SendMessageRequest>(p_RequestMessage);

        bool ok = SendLine("PRIVMSG " + sendMessageRequest->chatId + " :" +
                            sendMessageRequest->chatMessage.text);

        std::shared_ptr<SendMessageNotify> sendMessageNotify = std::make_shared<SendMessageNotify>(m_ProfileId);
        sendMessageNotify->success = ok;
        sendMessageNotify->chatId = sendMessageRequest->chatId;
        sendMessageNotify->chatMessage = sendMessageRequest->chatMessage;
        sendMessageNotify->chatMessage.isOutgoing = true;
        CallMessageHandler(sendMessageNotify);

        if (ok)
        {
          // No server echo of our own PRIVMSG without IRCv3 echo-message,
          // so build the self-sent message locally, same shape as an
          // incoming PRIVMSG in HandleLine(), instead of waiting on a
          // network confirmation the way sgchat does.
          ChatMessage chatMessage = sendMessageRequest->chatMessage;
          if (chatMessage.id.empty())
          {
            chatMessage.id = sendMessageRequest->chatId + "_" + std::to_string(time(nullptr)) + "_" + m_Nick;
          }
          chatMessage.senderId = m_Nick;
          chatMessage.isOutgoing = true;
          chatMessage.isRead = true;
          if (chatMessage.timeSent <= 0)
          {
            chatMessage.timeSent = static_cast<int64_t>(time(nullptr)) * 1000;
          }

          std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
          newMessagesNotify->success = true;
          newMessagesNotify->chatId = sendMessageRequest->chatId;
          newMessagesNotify->cached = false;
          newMessagesNotify->sequence = true;
          newMessagesNotify->chatMessages.push_back(chatMessage);
          CallMessageHandler(newMessagesNotify);
        }
      }
      break;

    case SetStatusRequestType:
      {
        std::shared_ptr<SetStatusRequest> setStatusRequest =
          std::static_pointer_cast<SetStatusRequest>(p_RequestMessage);

        // Rough mapping of online/offline onto IRC's AWAY mechanism.
        bool ok = setStatusRequest->isOnline ? SendLine("AWAY") : SendLine("AWAY :away");

        std::shared_ptr<SetStatusNotify> setStatusNotify = std::make_shared<SetStatusNotify>(m_ProfileId);
        setStatusNotify->success = ok;
        setStatusNotify->isOnline = setStatusRequest->isOnline;
        CallMessageHandler(setStatusNotify);
      }
      break;

    case DeferNotifyRequestType:
      {
        std::shared_ptr<DeferNotifyRequest> deferNotifyRequest =
          std::static_pointer_cast<DeferNotifyRequest>(p_RequestMessage);
        CallMessageHandler(deferNotifyRequest->serviceMessage);
      }
      break;

    case GetMessagesRequestType:
      {
        std::shared_ptr<GetMessagesRequest> getMessagesRequest =
          std::static_pointer_cast<GetMessagesRequest>(p_RequestMessage);

        MessageCache::FetchMessagesFrom(m_ProfileId, getMessagesRequest->chatId,
                                        getMessagesRequest->fromMsgId,
                                        getMessagesRequest->limit, false /* p_Sync */);
      }
      break;

    case MarkMessageReadRequestType:
      {
        std::shared_ptr<MarkMessageReadRequest> markMessageReadRequest =
          std::static_pointer_cast<MarkMessageReadRequest>(p_RequestMessage);

        // No server-side read-receipt concept in IRC — this is purely local
        // bookkeeping. MessageCache::AddFromServiceMessage handles
        // MarkMessageReadNotifyType by calling UpdateMessageIsRead itself,
        // so pushing this notify through CallMessageHandler is sufficient.
        std::shared_ptr<MarkMessageReadNotify> markMessageReadNotify =
          std::make_shared<MarkMessageReadNotify>(m_ProfileId);
        markMessageReadNotify->success = true;
        markMessageReadNotify->chatId = markMessageReadRequest->chatId;
        markMessageReadNotify->msgId = markMessageReadRequest->msgId;
        CallMessageHandler(markMessageReadNotify);
      }
      break;

    default:
      LOG_DEBUG("irc unhandled request type %d", p_RequestMessage->GetMessageType());
      break;
  }
}
