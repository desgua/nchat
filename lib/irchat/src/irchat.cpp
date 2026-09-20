// IrChat.cpp
//
// Copyright (c) 2020-2026 Kristofer Berggren
// irchat.cpp  by André Desgualdo Pereira (desgua, kana)
//
// All rights reserved.
//
// nchat is distributed under the MIT license, see LICENSE for details.
//
// Deliberately NOT covered yet (left as TODOs / return no-ops):
//   - TLS (plain-text socket only for now)
//   - SASL auth
//   - reactions, edit, pin, archive, delete, typing (no IRC equivalent)
//   - real Config class integration (using ad-hoc parsing for now)

#include "irchat.h"
#include "log.h"
#include "status.h"
#include "messagecache.h"

#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>
#include <cctype>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/time.h>

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

void IrChat::PostSystemMessage(const std::string& p_Text)
{
  static const std::string sysChatId = "irc info";

  EnsureChat(sysChatId, false);
  EnsureContact(sysChatId, "irc info");

  ChatMessage chatMessage;
  chatMessage.id = sysChatId + "_" + std::to_string(time(nullptr)) + "_" +
                    std::to_string(m_SysMsgCounter++);
  chatMessage.senderId = sysChatId;
  chatMessage.text = p_Text;
  chatMessage.timeSent = static_cast<int64_t>(time(nullptr)) * 1000;
  chatMessage.isOutgoing = false;
  chatMessage.isRead = false;

  std::shared_ptr<NewMessagesNotify> newMessagesNotify = std::make_shared<NewMessagesNotify>(m_ProfileId);
  newMessagesNotify->success = true;
  newMessagesNotify->chatId = sysChatId;
  newMessagesNotify->cached = false;
  newMessagesNotify->sequence = true;
  newMessagesNotify->chatMessages.push_back(chatMessage);
  CallMessageHandler(newMessagesNotify);
}

static bool ConfigureSocketKeepAlive(int sock)
{
  // Enable TCP keepalive
  int optval = 1;
  socklen_t optlen = sizeof(optval);
  if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen) < 0)
  {
    LOG_DEBUG("irc setsockopt SO_KEEPALIVE failed: %s", strerror(errno));
    return false;
  }

  // Set Idle time before sending first keepalive probe
  int idleTime = 30;
  // Linux defines TCP_KEEPIDLE and MacOS defines TCP_KEEPALIVE
#if defined(TCP_KEEPIDLE)
  if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idleTime, sizeof(idleTime)) < 0)
  {
    LOG_DEBUG("irc setsockopt TCP_KEEPIDLE failed: %s", strerror(errno));
  }
#elif defined(TCP_KEEPALIVE)
  if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPALIVE, &idleTime, sizeof(idleTime)) < 0)
  {
    LOG_DEBUG("irc setsockopt TCP_KEEPALIVE failed: %s", strerror(errno));
  }
#endif

  // Interval between retry probes if no response
  int interval = 10;
  if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) < 0)
  {
    LOG_DEBUG("irc setsockopt TCP_KEEPINTVL failed: %s", strerror(errno));
  }

  // Maximum number of failed probes before dropping connection
  int count = 3;
  if (setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) < 0)
  {
    LOG_DEBUG("irc setsockopt TCP_KEEPCNT failed: %s", strerror(errno));
  }

  // Set socket receive timeout
  // This ensures a blocking recv() will unblock periodically if the interface drops.
  struct timeval tv;
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

  return true;
}

static std::string IrcToMarkdown(const std::string& text)
{
  std::string result;
  result.reserve(text.size());

  bool inBold = false;
  bool inItalic = false;
  bool inStrike = false;
  bool inCode = false;

  size_t len = text.size();
  for (size_t i = 0; i < len; ++i)
  {
    unsigned char c = text[i];

    // ^A (\x01): Action -> we will treat it as bold
    if (c == '\x01')
    {
      result += "*";
      inBold = !inBold;
      continue;
    }


    // ^B (\x02): Bold toggle
    if (c == '\x02')
    {
      result += "*";
      inBold = !inBold;
      continue;
    }

    // ^] (\x1D): Italic toggle
    if (c == '\x1D')
    {
      result += "_";
      inItalic = !inItalic;
      continue;
    }

    // ^_ (\x1F): Underline (Markdown doesn't have standard underline; map to italic or ignore)
    if (c == '\x1F')
    {
      result += "_";
      inItalic = !inItalic;
      continue;
    }

    // ^~ (\x1E): Strikethrough toggle
    if (c == '\x1E')
    {
      result += "~";
      inStrike = !inStrike;
      continue;
    }

    // ^Q (\x11): Monospace / Fixed-width font toggle
    if (c == '\x11')
    {
      result += "`";
      inCode = !inCode;
      continue;
    }

    // ^O (\x0F): Reset all formatting
    if (c == '\x0F')
    {
      if (inCode)   { result += "`"; inCode = false; }
      if (inStrike) { result += "~"; inStrike = false; }
      if (inItalic) { result += "_"; inItalic = false; }
      if (inBold)   { result += "*"; inBold = false; }
      continue;
    }

    // ^C (\x03): Color code \x03[FG][,BG]
    // Consumes colors so digits like \x0304 don't leak into the message
    if (c == '\x03')
    {
      // Optional 1-2 digits for foreground color
      if (i + 1 < len && isdigit(text[i + 1]))
      {
        i++;
        if (i + 1 < len && isdigit(text[i + 1]))
        {
          i++;
        }
        // Optional comma and 1-2 digits for background color
        if (i + 1 < len && text[i + 1] == ',')
        {
          if (i + 2 < len && isdigit(text[i + 2]))
          {
            i += 2;
            if (i + 1 < len && isdigit(text[i + 1]))
            {
              i++;
            }
          }
        }
      }
      continue;
    }

    // ^[ (\x1B): ANSI Escape Sequences (\x1B[...m)
    if (c == '\x1B')
    {
      if (i + 1 < len && text[i + 1] == '[')
      {
        size_t j = i + 2;
        while (j < len && (isdigit(text[j]) || text[j] == ';'))
        {
          j++;
        }
        if (j < len && text[j] == 'm')
        {
          std::string codeStr = text.substr(i + 2, j - (i + 2));
          std::stringstream ss(codeStr);
          std::string token;

          if (codeStr.empty()) // \x1B[m is equivalent to \x1B[0m (reset)
          {
            if (inCode)   { result += "`"; inCode = false; }
            if (inStrike) { result += "~"; inStrike = false; }
            if (inItalic) { result += "_"; inItalic = false; }
            if (inBold)   { result += "*"; inBold = false; }
          }

          while (std::getline(ss, token, ';'))
          {
            int code = token.empty() ? 0 : std::stoi(token);
            if (code == 0) // Reset
            {
              if (inCode)   { result += "`"; inCode = false; }
              if (inStrike) { result += "~"; inStrike = false; }
              if (inItalic) { result += "_"; inItalic = false; }
              if (inBold)   { result += "*"; inBold = false; }
            }
            else if (code == 1 && !inBold) // Bold ON
            {
              result += "*";
              inBold = true;
            }
            else if (code == 22 && inBold) // Bold OFF
            {
              result += "*";
              inBold = false;
            }
            else if (code == 3 && !inItalic) // Italic ON
            {
              result += "_";
              inItalic = true;
            }
            else if (code == 23 && inItalic) // Italic OFF
            {
              result += "_";
              inItalic = false;
            }
          }
          i = j; // Advance past the ANSI sequence
          continue;
        }
      }
    }

    // Normal character
    result += c;
  }

  // In IRC, authors often don't close tags before the end of the line.
  // We must close all active markdown tags so nchat renders them properly.
  if (inCode)   result += "`";
  if (inStrike) result += "~";
  if (inItalic) result += "_";
  if (inBold)   result += "*";

  return result;
}

static std::string MarkdownToIrc(const std::string& text)
{
  std::string result;
  bool inBold = false;
  bool inItalic = false;

  for (size_t i = 0; i < text.size(); ++i)
  {
    if (text[i] == '*' && (i == 0 || text[i-1] != '\\'))
    {
      result += "\x02"; // ^B
      inBold = !inBold;
    }
    else if (text[i] == '_' && (i == 0 || text[i-1] != '\\'))
    {
      result += "\x1D"; // ^]
      inItalic = !inItalic;
    }
    else
    {
      result += text[i];
    }
  }

  // If unclosed, append reset
  if (inBold || inItalic)
  {
    result += "\x0F";
  }

  return result;
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
      ConfigureSocketKeepAlive(sock);
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

  {
  std::unique_lock<std::mutex> lock(m_SocketMutex);
  m_Socket = sock;
  }

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
      EnsureContact(channel, channel);
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
    std::string text = IrcToMarkdown(msg.params[1]);
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

  if (msg.command == "353") // RPL_NAMREPLY
  {
    if (msg.params.size() < 4) return;
    std::string channel = msg.params[2];
    std::istringstream iss(msg.params[3]);
    std::string nick;
    while (iss >> nick)
    {
      // Strip channel-role prefixes (@ op, + voice, etc.)
      while (!nick.empty() && (nick[0] == '@' || nick[0] == '+' || nick[0] == '%'))
      {
        nick.erase(0, 1);
      }
      if (nick.empty()) continue;

      EnsureContact(nick, nick);
      m_PendingNames[channel].insert(nick);
    }
    return;
  }

  if (msg.command == "366") // RPL_ENDOFNAMES
  {
    if (msg.params.size() < 2) return;
    std::string channel = msg.params[1];

    auto it = m_PendingNames.find(channel);
    if (it != m_PendingNames.end())
    {
      std::shared_ptr<NewGroupMembersNotify> newGroupMembersNotify =
        std::make_shared<NewGroupMembersNotify>(m_ProfileId);
      newGroupMembersNotify->chatId = channel;
      for (const std::string& nick : it->second)
      {
        ContactInfo contactInfo;
        // Prefixed so id != name — the shared group-member dialog
        // (uigroupmemberlistdialog.cpp) filters out entries where
        // name == memberId, assuming that means "unresolved contact"
        // (true for WhatsApp/Signal's phone-number/UUID ids, but IRC's
        // id IS the nick by design). This id is scoped to this feature
        // only — never used as a JOIN/PRIVMSG target or matched against
        // chatId/senderId anywhere, so it's safe to differ from the
        // nick used elsewhere for messaging and sender-name display.
        contactInfo.id = "member:" + nick;
        contactInfo.name = nick;
        newGroupMembersNotify->contactInfos.push_back(contactInfo);
      }
      CallMessageHandler(newGroupMembersNotify);
      PostSystemMessage("Total of " + std::to_string(it->second.size()) + " nicks in " + channel);
      m_PendingNames.erase(it);
    }
    return;
  }

  if (msg.command == "332") // RPL_TOPIC
  {
    if (msg.params.size() < 3) return;
    PostSystemMessage("Topic for " + msg.params[1] + ": " + msg.params[2]);
    return;
  }

  if (msg.command == "333") // RPL_TOPICWHOTIME
  {
    if (msg.params.size() < 4) return;
    std::string setter = msg.params[2];
    time_t setTime = static_cast<time_t>(std::strtoll(msg.params[3].c_str(), nullptr, 10));
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", localtime(&setTime));
    PostSystemMessage("Topic for " + msg.params[1] + " set by " + setter + " [" + buf + "]");
    return;
  }

  if (msg.command == "329") // RPL_CREATIONTIME
  {
    if (msg.params.size() < 3) return;
    time_t createTime = static_cast<time_t>(std::strtoll(msg.params[2].c_str(), nullptr, 10));
    char buf[64];
    strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y", localtime(&createTime));
    PostSystemMessage("Channel " + msg.params[1] + " created " + std::string(buf));
    return;
  }

  if (msg.command == "NOTICE")
  {
    if (msg.params.size() < 2) return;
    std::string from = PrefixToNick(msg.prefix);
    PostSystemMessage((from.empty() ? "" : "[" + from + "] ") + msg.params[1]);
    return;
  }

  if ((msg.command == "473") || (msg.command == "474") || (msg.command == "475") ||
      (msg.command == "477") || (msg.command == "471") || (msg.command == "403"))
  {
    std::string detail = msg.params.empty() ? "" : msg.params.back();
    LOG_DEBUG("irc join failed (%s): %s", msg.command.c_str(), detail.c_str());
    return;
  }

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

  time_t lastRecvTime = time(nullptr);
  bool pingSent = false;

  const int pingIdleSec = 60;    // Send PING after 60s of silence
  const int pingTimeoutSec = 90; // Dead connection if no response in 90s (or after resume)

  while (m_Running)
  {
    ssize_t n = recv(m_Socket, recvBuf, sizeof(recvBuf), 0);
    if (n < 0)
    {
      if (errno == EINTR)
      {
        continue;
      }

      if ((errno == EAGAIN) || (errno == EWOULDBLOCK))
      {
        time_t now = time(nullptr);
        int idleSec = static_cast<int>(now - lastRecvTime);

        // 1. If laptop woke up from sleep or server went silent > 90s, disconnect & reconnect
        if (idleSec >= pingTimeoutSec)
        {
          LOG_DEBUG("irc connection timed out (%ds idle, suspend/dead connection). Reconnecting...", idleSec);
          return;
        }

        // 2. If idle > 60s, send application-level PING
        if (idleSec >= pingIdleSec && !pingSent)
        {
          LOG_DEBUG("irc sending keepalive ping");
          SendLine("PING :" + std::to_string(now));
          pingSent = true;
        }

        continue;
      }

      LOG_DEBUG("irc recv error: %s", strerror(errno));
      return;
    }
    else if (n == 0)
    {
      LOG_DEBUG("irc connection closed by peer");
      return;
    }

    // Reset idle timers on any incoming data from server
    lastRecvTime = time(nullptr);
    pingSent = false;

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
      std::vector<std::string> groupChatIds;
      for (const auto& chat : m_KnownChats)
      {
        ChatInfo chatInfo;
        chatInfo.id = chat.first;
        newChatsNotify->chatInfos.push_back(chatInfo);
      }
      lock.unlock();

      CallMessageHandler(newChatsNotify);

      // Channels ride along as regular ContactInfo entries too (mirrors
      // wmchat, where WhatsApp groups are pushed through the same generic
      // contact callback as 1:1 contacts) so they're selectable from the
      // "Open Chat" dialog, not just reachable via the chat switcher.
      for (const std::string& channel : groupChatIds)
      {
        EnsureContact(channel, channel);
      }
    }
    break;

    case SendMessageRequestType:
      {
        std::shared_ptr<SendMessageRequest> sendMessageRequest =
          std::static_pointer_cast<SendMessageRequest>(p_RequestMessage);

        std::string targetChatId = sendMessageRequest->chatId;
        std::string textToSend = sendMessageRequest->chatMessage.text;
        bool isSlashMsgOrQuery = false;

        // Check for /msg <nick> [text] or /query <nick> [text]
        if (textToSend.rfind("/msg ", 0) == 0 || textToSend.rfind("/query ", 0) == 0)
        {
          isSlashMsgOrQuery = true;
          size_t firstSpace = textToSend.find(' ');
          size_t secondSpace = textToSend.find(' ', firstSpace + 1);

          if (secondSpace == std::string::npos)
          {
            // "/msg nick" or "/query nick" without text
            targetChatId = textToSend.substr(firstSpace + 1);
            textToSend = "";
          }
          else
          {
            // "/msg nick some text here"
            targetChatId = textToSend.substr(firstSpace + 1, secondSpace - (firstSpace + 1));
            size_t textStart = textToSend.find_first_not_of(' ', secondSpace);
            textToSend = (textStart != std::string::npos) ? textToSend.substr(textStart) : "";
          }

          // Strip any mode prefixes like @, +, %
          if (!targetChatId.empty() && (targetChatId[0] == '@' || targetChatId[0] == '+' || targetChatId[0] == '%'))
          {
            targetChatId.erase(0, 1);
          }

          if (targetChatId.empty())
          {
            LOG_DEBUG("irc /msg or /query missing target nickname");
            std::shared_ptr<SendMessageNotify> notify = std::make_shared<SendMessageNotify>(m_ProfileId);
            notify->success = false;
            notify->chatId = sendMessageRequest->chatId;
            notify->chatMessage = sendMessageRequest->chatMessage;
            CallMessageHandler(notify);
            break;
          }

          // Register DM chat and contact with nchat
          EnsureChat(targetChatId, false /* p_IsGroup */);
          EnsureContact(targetChatId, targetChatId);

          // If /query with no payload, acknowledge success so nchat clears the input
          if (textToSend.empty())
          {
            std::shared_ptr<SendMessageNotify> notify = std::make_shared<SendMessageNotify>(m_ProfileId);
            notify->success = true;
            notify->chatId = sendMessageRequest->chatId; // Must match origin to clear input
            notify->chatMessage = sendMessageRequest->chatMessage;
            notify->chatMessage.isOutgoing = true;
            CallMessageHandler(notify);
            break;
          }
        }

        // Send over IRC socket
        std::string ircPayload = MarkdownToIrc(textToSend);
        bool ok = SendLine("PRIVMSG " + targetChatId + " :" + ircPayload);

        // 1. Notify the original chat request -> clears the active input box
        std::shared_ptr<SendMessageNotify> sendMessageNotify = std::make_shared<SendMessageNotify>(m_ProfileId);
        sendMessageNotify->success = ok;
        sendMessageNotify->chatId = sendMessageRequest->chatId; // Always match original request
        sendMessageNotify->chatMessage = sendMessageRequest->chatMessage;
        sendMessageNotify->chatMessage.isOutgoing = true;
        CallMessageHandler(sendMessageNotify);

        // 2. Put the sent message into the target chat history
        if (ok)
        {
          ChatMessage chatMessage = sendMessageRequest->chatMessage;
          chatMessage.text = textToSend;
          if (chatMessage.id.empty() || isSlashMsgOrQuery)
          {
            chatMessage.id = targetChatId + "_" + std::to_string(time(nullptr)) + "_" + m_Nick;
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
          newMessagesNotify->chatId = targetChatId; // Routes message to recipient window
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

    case GetGroupMembersRequestType:
      {
        std::shared_ptr<GetGroupMembersRequest> getGroupMembersRequest =
          std::static_pointer_cast<GetGroupMembersRequest>(p_RequestMessage);
        SendLine("NAMES " + getGroupMembersRequest->chatId);
      }
      break;

    case DeleteChatRequestType:
      {
        std::shared_ptr<DeleteChatRequest> deleteChatRequest =
          std::static_pointer_cast<DeleteChatRequest>(p_RequestMessage);
        std::string chatId = deleteChatRequest->chatId;

        bool isGroup = IsGroupChat(chatId);
        {
          std::unique_lock<std::mutex> lock(m_ChatsMutex);
          auto it = m_KnownChats.find(chatId);
          if (it != m_KnownChats.end())
          {
            if (isGroup)
            {
              lock.unlock();
              SendLine("PART " + chatId);
              lock.lock();
            }
            m_KnownChats.erase(it);
          }
          m_KnownNicks.erase(chatId);
        }

        std::shared_ptr<DeleteChatNotify> deleteChatNotify = std::make_shared<DeleteChatNotify>(m_ProfileId);
        deleteChatNotify->success = true;
        deleteChatNotify->chatId = chatId;
        CallMessageHandler(deleteChatNotify);
      }
      break;

    default:
      LOG_DEBUG("irc unhandled request type %d", p_RequestMessage->GetMessageType());
      break;
  }
}
