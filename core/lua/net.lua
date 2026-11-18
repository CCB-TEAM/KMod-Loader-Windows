-- kmod `Net`: watch and change the game's own WebSocket traffic (built-in script; per-mod table built by kmod.net_api).
-- Needs "permissions": ["game_network"] in mod.json. HTTP requests of the game are native code and not reachable from here;
-- to talk to another server, see the server redirect in docs (kmod/net/server.json).
--
-- The game's WebSocketSubsystem delivers incoming messages to several listeners with OnWebSocketMessageReceived(Channel,
-- Sender, Message, Context): a delegate broadcast, which goes through ProcessEvent, so a hook sees the strings and can replace
-- them. What the game SENDS (SendWebSocketMessage) and OpenWebSocket are native functions the Blueprint VM calls directly,
-- bypassing ProcessEvent: the core puts a thunk on them (see core.cpp, native function hooks) so on_send/on_open see them.

local function log(...) kmod.log("info", "net", ...) end

local FN = {
  -- every listener of the subsystem's OnWebSocketMessageReceived delegate; a message reaches all of them, one after the other
  receive = { "BP_KardsSession_C:OnWebSocketMessageReceived", "FrontPageSubsystem:OnWebSocketMessageReceived",
              "StoreSubsystem:Internal_OnWebSocketMessageReceived" },
}

local function str(v) if v == nil then return "" end return tostring(v) end

local function readMsg(p, who)
  return { channel = str(p.Channel), [who] = str(p[who == "sender" and "Sender" or "Receiver"]), message = str(p.Message), context = str(p.Context) }
end

local function applyMsg(p, who, new)
  local pk = who == "sender" and "Sender" or "Receiver"
  if new.channel ~= nil then p.Channel = tostring(new.channel) end
  if new[who] ~= nil then p[pk] = tostring(new[who]) end
  if new.message ~= nil then p.Message = tostring(new.message) end
  if new.context ~= nil then p.Context = tostring(new.context) end
end

local function sameMsg(a, b, who)
  return a.channel == b.channel and a[who] == b[who] and a.message == b.message and a.context == b.context
end

-- call a mod's handler: errors are logged and count against the mod
local function call(owner, fn, ...)
  kmod.set_activity("mod", owner, "net")
  local ok, r = pcall(fn, ...)
  kmod.set_activity()
  if not ok then
    kmod.log("error", owner, "Net handler failed: " .. tostring(r))
    if kmod._mod_error then kmod._mod_error(owner, tostring(r)) end
    return nil
  end
  return r
end

function kmod.net_api(rec)
  local denied = function() error('Net: this mod must declare "permissions": ["game_network"] in mod.json', 3) end
  if not rec.permissions.game_network then
    return setmetatable({}, { __index = function() return denied end })
  end
  local api = kmod.mod_api(rec.id)                      -- owner-tagged RegisterHook: removed when the mod is disabled/reloaded
  local Net = {}

  -- Net.on_message(fn): fn(msg) for every message the game receives; msg = { channel, sender, message, context }.
  -- Return a table to replace fields. The message is handed to several listeners one after the other: fn runs once per message
  -- and every listener gets the changed text. The game keeps that parameter memory for the next message, so each listener's
  -- call is followed by putting the original text back (post hook): a change never leaks into the next message.
  function Net.on_message(fn)
    local last                                           -- the message being delivered: { orig, repl, t }
    local function pre(self, p)
      local m = readMsg(p, "sender")
      local now = os.clock()
      if last and now - last.t < 0.05 and sameMsg(last.orig, m, "sender") then     -- another listener of the same message
        last.t = now
        if last.repl then applyMsg(p, "sender", last.repl) end
        return
      end
      local orig = {}
      for k, v in pairs(m) do orig[k] = v end
      local r = call(rec.id, fn, m)
      last = { orig = orig, repl = type(r) == "table" and r or nil, t = now }
      if last.repl then applyMsg(p, "sender", last.repl) end
    end
    local function post(self, p)
      if last and last.repl then applyMsg(p, "sender", last.orig) end
    end
    for _, path in ipairs(FN.receive) do
      local ok, err = pcall(api.RegisterHook, path, pre, post)
      if not ok then log("could not watch " .. path .. ": " .. tostring(err)) end
    end
  end

  -- Net.on_send(fn): fn(msg) for every message the game SENDS; msg = { channel, receiver, message, context }.
  -- Return a table to replace fields before the message goes out. (Messages sent with Net.send are not reported.)
  function Net.on_send(fn)
    local ok, err = pcall(api.RegisterHook, "WebSocketSubsystem:SendWebSocketMessage", function(self, p)
      local m = readMsg(p, "receiver")
      local r = call(rec.id, fn, m)
      if type(r) == "table" then applyMsg(p, "receiver", r) end
    end)
    if not ok then log("could not watch SendWebSocketMessage: " .. tostring(err)) end
  end

  -- Net.on_open(fn): fn({ url, token }) when the game opens its WebSocket. Return a table to change the URL or token.
  function Net.on_open(fn)
    local ok, err = pcall(api.RegisterHook, "WebSocketSubsystem:OpenWebSocket", function(self, p)
      local m = { url = str(p.URL), token = str(p.Token) }
      local r = call(rec.id, fn, m)
      if type(r) == "table" then
        if r.url ~= nil then p.URL = tostring(r.url) end
        if r.token ~= nil then p.Token = tostring(r.token) end
      end
    end)
    if not ok then log("could not watch OpenWebSocket: " .. tostring(err)) end
  end

  -- Net.on_state(fn): fn(state [, detail]) with state "open", "closed" or "failure" (detail = the error text)
  function Net.on_state(fn)
    local paths = { open = "BP_KardsSession_C:OnWebSocketOpen", closed = "BP_KardsSession_C:OnWebSocketClosed",
                    failure = "BP_KardsSession_C:OnWebSocketFailure" }
    for state, path in pairs(paths) do
      api.RegisterHook(path, function(self, p)
        local detail = nil
        if state == "failure" then detail = str(p.Error) end
        call(rec.id, fn, state, detail)
      end)
    end
  end

  -- the URL of the game's current WebSocket ("" when there is none)
  function Net.url()
    local sub = FindFirstOf("WebSocketSubsystem")
    return (sub and sub:IsValid()) and str(sub.WebSocketUrl) or ""
  end

  -- Net.send(channel, receiver, message [, context]): send a message through the game's own connection
  function Net.send(channel, receiver, message, context)
    local sub = FindFirstOf("WebSocketSubsystem")
    if not (sub and sub:IsValid()) then error("Net.send: the game's WebSocket subsystem is not available", 2) end
    sub:SendWebSocketMessage(channel or "", receiver or "", message or "", context or "")
  end

  function Net.is_connected()
    local sub = FindFirstOf("WebSocketSubsystem")
    return sub ~= nil and sub:IsValid() and sub:IsConnected() == true
  end

  -- the server redirect (kmod/net/server.json): what was done at start-up
  function Net.redirect_status() return kmod.net_redirect_status() end
  return Net
end

-- ------------------------------------------------------------------ sockets and WebSocket clients for mods
-- WebSocket.connect(url, handlers) / Socket.tcp(host, port, handlers) / Socket.udp(host, port, handlers). Needs the "network"
-- permission. Each connection runs on its own thread (net_conn.cpp); handlers are called on the game thread.
kmod._net_conns = kmod._net_conns or {}

function kmod._net_event(id, event, data, extra, binary)
  local c = kmod._net_conns[id]
  if not c then return end
  local h = c.handlers
  local function run(fn, ...)
    if type(fn) ~= "function" then return end
    kmod.set_activity("mod", c.owner, "net")
    local ok, err = pcall(fn, ...)
    kmod.set_activity()
    if not ok then
      kmod.log("error", c.owner, c.kind .. " handler failed: " .. tostring(err))
      if kmod._mod_error then kmod._mod_error(c.owner, tostring(err)) end
    end
  end
  if event == "open" then c.open = true; run(h.on_open)
  elseif event == "message" then run(h.on_message, data, binary)
  elseif event == "data" then run(h.on_data, data)
  elseif event == "error" then run(h.on_error, data)
  elseif event == "close" then
    kmod._net_conns[id] = nil
    c.closed = true
    run(h.on_close, tonumber(extra) or 0, data)
  end
end

function kmod.net_sockets_api(rec)
  local denied = function() error('this mod must declare "permissions": ["network"] in mod.json', 3) end
  if not rec.permissions.network then
    local stub = setmetatable({}, { __index = function() return denied end })
    return stub, stub
  end
  local function open(kind, opts, handlers)
    handlers = handlers or {}
    local id = kmod.net_open(rec.id, kind, opts)
    local conn = { owner = rec.id, kind = kind, handlers = handlers }
    kmod._net_conns[id] = conn
    local obj = { id = id }
    function obj:send(data) return kmod.net_send(id, tostring(data), false) end
    function obj:send_binary(data) return kmod.net_send(id, tostring(data), true) end
    function obj:close() kmod.net_close(id) end
    function obj:is_open() return conn.open == true and not conn.closed end
    return obj
  end
  local WS, Sock = {}, {}
  -- WebSocket.connect(url, { headers = {}, timeout = ms, on_open = fn(), on_message = fn(text, is_binary), on_close = fn(code, reason), on_error = fn(msg) })
  function WS.connect(url, handlers)
    local opts = { url = url, headers = handlers and handlers.headers, timeout = handlers and handlers.timeout }
    return open("ws", opts, handlers)
  end
  -- Socket.tcp(host, port, { tls = bool, on_open = fn(), on_data = fn(bytes), on_close = fn(code, reason), on_error = fn(msg) })
  function Sock.tcp(host, port, handlers)
    return open("tcp", { host = host, port = port, tls = handlers and handlers.tls, timeout = handlers and handlers.timeout }, handlers)
  end
  -- Socket.udp(host, port, { on_data = fn(bytes), ... }): a connected UDP socket, obj:send(bytes)
  function Sock.udp(host, port, handlers)
    return open("udp", { host = host, port = port }, handlers)
  end
  return WS, Sock
end
