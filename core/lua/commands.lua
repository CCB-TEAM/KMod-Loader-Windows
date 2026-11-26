-- kmod console commands: a registry with built-in commands. Mods add their own with Console.register (see docs/console.md).
-- Entry point for the host: kmod.console_exec(line) -> true when the line was a command (otherwise the host runs it as Lua).
-- Re-running this file (dofile) is safe: it keeps registered commands.

local cmds = kmod._cmds or {}              -- name -> { fn, help, usage, owner }
kmod._cmds = cmds

local NAME_PAT = "^[a-z][a-z0-9_%-%.]*$"

-- split a command line into tokens; "double" and 'single' quotes group words, backslash escapes inside quotes
local function tokenize(line)
  local out, i, n = {}, 1, #line
  while i <= n do
    local c = line:sub(i, i)
    if c:match("%s") then i = i + 1
    elseif c == '"' or c == "'" then
      local q, buf = c, {}
      i = i + 1
      while i <= n and line:sub(i, i) ~= q do
        local ch = line:sub(i, i)
        if ch == "\\" and i < n then i = i + 1; ch = line:sub(i, i) end
        buf[#buf + 1] = ch
        i = i + 1
      end
      i = i + 1
      out[#out + 1] = table.concat(buf)
    else
      local j = i
      while j <= n and not line:sub(j, j):match("%s") do j = j + 1 end
      out[#out + 1] = line:sub(i, j - 1)
      i = j
    end
  end
  return out
end
kmod.console_tokenize = tokenize

local function register(name, fn, info, owner)
  if type(name) ~= "string" or not name:match(NAME_PAT) then return false, "invalid command name (use a-z 0-9 _ - .)" end
  if type(fn) ~= "function" then return false, "handler must be a function" end
  local old = cmds[name]
  if old and old.owner ~= owner then return false, "command '" .. name .. "' is already registered" .. (old.owner ~= "" and (" by " .. old.owner) or " (built-in)") end
  local help, usage = "", nil
  if type(info) == "string" then help = info
  elseif type(info) == "table" then help, usage = info.help or "", info.usage end
  cmds[name] = { fn = fn, help = help, usage = usage, owner = owner }
  return true
end

local function run(name, args, line)
  local c = cmds[name]
  local ctx = { line = line, print = print }
  local ok, res = pcall(c.fn, args, ctx)
  if not ok then
    print("[error] " .. name .. ": " .. tostring(res):match("^[^\n]*"))
    kmod.log("warn", c.owner ~= "" and c.owner or "console", "command '" .. name .. "' failed: " .. tostring(res))
  elseif res ~= nil then
    print(tostring(res))
  end
end

-- explicit: the line started with "/" (always a command); otherwise only a registered first word that is not followed by
-- Lua syntax ("mods = 1", "reload(...)") counts as a command.
function kmod.console_exec(line)
  line = tostring(line or ""):match("^%s*(.-)%s*$")
  local explicit = line:sub(1, 1) == "/"
  if explicit then line = line:sub(2) end
  local name = line:match("^(%S+)")
  if not name then return explicit end
  if not cmds[name] then
    if explicit then print("unknown command '" .. name .. "' (try: help)") end
    return explicit
  end
  local rest = line:sub(#name + 1)
  if not explicit and rest:match("^%s*[=%.%(:%[]") then return false end
  local args = tokenize(rest)
  run(name, args, line)
  return true
end

-- API handed to each mod as `Console`; commands are dropped when the mods reload
function kmod.console_api(rec)
  return {
    register = function(name, fn, info)
      local ok, err = register(name, fn, info, rec.id)
      if not ok then kmod.log("warn", rec.id, "Console.register('" .. tostring(name) .. "'): " .. tostring(err)) end
      return ok, err
    end,
    unregister = function(name)
      local c = cmds[name]
      if c and c.owner == rec.id then cmds[name] = nil; return true end
      return false
    end,
    exec = function(line) return kmod.console_exec(line) end,
    print = function(...) print(...) end,
  }
end

function kmod.console_clear_mods(only)
  for name, c in pairs(cmds) do if c.owner ~= "" and (not only or c.owner == only) then cmds[name] = nil end end
end

local function builtin(name, fn, help, usage) register(name, fn, { help = help, usage = usage }, "") end

local function findMod(id)
  for pass = 1, 2 do
    for _, rec in ipairs(kmod.mods or {}) do if rec.id == id then return rec end end
    if pass == 1 then kmod.refresh_mods() end          -- the mod may have been copied in after the last scan
  end
end

-- ------------------------------------------------------------------ built-in commands
builtin("help", function(args)
  if args[1] then
    local c = cmds[args[1]]
    if not c then return "unknown command '" .. args[1] .. "'" end
    local s = args[1] .. (c.owner ~= "" and ("  [" .. c.owner .. "]") or "") .. "\n  " .. (c.help ~= "" and c.help or "(no description)")
    if c.usage then s = s .. "\n  usage: " .. c.usage end
    return s
  end
  local names = {}
  for name in pairs(cmds) do names[#names + 1] = name end
  table.sort(names, function(a, b)
    local oa, ob = cmds[a].owner ~= "", cmds[b].owner ~= ""
    if oa ~= ob then return not oa end
    return a < b
  end)
  local lines = { "Commands (anything else is run as Lua; prefix a line with / to force a command):" }
  for _, name in ipairs(names) do
    local c = cmds[name]
    lines[#lines + 1] = string.format("  %-14s %s%s", name, c.help, c.owner ~= "" and ("  [" .. c.owner .. "]") or "")
  end
  return table.concat(lines, "\n")
end, "list commands, or describe one", "help [command]")

builtin("version", function()
  return string.format("kmod %s (%s), api %d, game %s", kmod.version, kmod.flavor, kmod.api, tostring(kmod.game_version or kmod.game or "unknown"))
end, "show loader and game versions")

builtin("mods", function()
  local list = kmod.refresh_mods()
  if #list == 0 then return "no mods installed" end
  local lines = {}
  for _, rec in ipairs(list) do
    lines[#lines + 1] = string.format("  %-20s %-10s %s", rec.id, rec.version ~= "" and rec.version or "-", rec.status or "")
  end
  return table.concat(lines, "\n")
end, "list installed mods and their status")

builtin("selftest", function()
  local lines, bad = {}, 0
  local function check(name, ok, detail)
    if not ok then bad = bad + 1 end
    lines[#lines + 1] = string.format("  [%s] %s%s", ok and "ok" or "FAIL", name, detail and (" - " .. tostring(detail)) or "")
  end
  check("kmod version", kmod.version ~= nil, kmod.version)
  check("game version", kmod.game ~= nil, kmod.game_version)
  local pc = FindFirstOf("PlayerController")
  check("find PlayerController", pc ~= nil and pc:IsValid())
  local lib = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
  check("find default object", lib ~= nil and lib:IsValid())
  local ok, name = pcall(function() return lib:GetGameName() end)
  check("call a UFunction (GetGameName)", ok and type(name) == "string" and name ~= "", ok and name or tostring(name))
  local j = kmod.json.decode(kmod.json.encode({ a = 1, b = { true, "x" } }))
  check("json round trip", j and j.a == 1 and j.b[2] == "x")
  check("object handle stays valid", pc ~= nil and pc:IsValid() and pc:GetFullName() ~= "")
  local b = kmod.refresh_boot()
  check("safe mode off", not b.safe)
  local st = kmod.pak_status()
  local bad_paks = {}
  for _, p in ipairs(st) do if p.status == "failed" then bad_paks[#bad_paks + 1] = p.name end end
  check("paks", #bad_paks == 0, #st .. " file(s)" .. (#bad_paks > 0 and (", failed: " .. table.concat(bad_paks, ", ")) or ""))
  local done
  local ok2, w = pcall(function()
    local path = kmod.paths.root .. "/boot/selftest_worker.lua"
    local f = io.open(path, "wb"); f:write("onMessage(function(m) post({sum = m.a + m.b, j = json.encode({m.a})}) end)"); f:close()
    local wk = kmod.worker("selftest", path)
    wk:onMessage(function(m) done = m; wk:close() end)
    wk:post({ a = 20, b = 22 })
    return wk
  end)
  check("worker started", ok2, ok2 and "reply arrives within a second" or tostring(w))
  if ok2 then
    ExecuteWithDelay(1500, function()
      local good = done and done.sum == 42 and done.j == "[20]"
      print("[selftest] worker round trip: " .. (good and "ok" or "FAIL"))
    end)
  end
  return table.concat(lines, "\n") .. "\n" .. (bad == 0 and "all checks passed" or (bad .. " check(s) failed"))
end, "run quick health checks of the loader (objects, calls, json, paks, worker)")

builtin("layout", function(args)
  if args[1] == "redetect" then
    kmod.detect_layout(true)
    return "detecting the engine layout again in the background (about 15 s); run 'layout' for the result"
  end
  return kmod.layout_report()
end, "show whether the engine's structure layout was verified against this kmod build; 'layout redetect' runs the detector again", "layout [redetect]")

builtin("safe", function(args)
  local b = kmod.refresh_boot()
  if args[1] == "off" then
    kmod.safe_off()
    kmod.refresh_boot()
    kmod.mount_paks()
    kmod.reload_mods()
    return "safe mode is off; mods reloaded"
  end
  local mods, paks = {}, {}
  for id in pairs(b.mods) do mods[#mods + 1] = id end
  for n in pairs(b.paks) do paks[#paks + 1] = n end
  table.sort(mods); table.sort(paks)
  return string.format("safe mode: %s\ncrashes in a row with no known cause: %d\nlast crash while running: %s\nquarantined mods: %s\nquarantined paks: %s",
    b.safe and "ON (mods and paks are not loaded)" or "off", b.crashes, b.last_crash ~= "" and b.last_crash or "-",
    #mods > 0 and table.concat(mods, ", ") or "-", #paks > 0 and table.concat(paks, ", ") or "-")
end, "show crash recovery state; 'safe off' leaves safe mode", "safe [off]")

builtin("unquarantine", function(args)
  if (args[1] ~= "mod" and args[1] ~= "pak") or not args[2] then return "usage: unquarantine <mod|pak> <name>" end
  kmod.unquarantine(args[1], args[2])
  kmod.refresh_boot()
  if args[1] == "mod" then kmod.reload_mods() else kmod.mount_paks() end
  return args[1] .. " " .. args[2] .. " can load again"
end, "allow a mod or pak that crashed the game to load again", "unquarantine <mod|pak> <name>")

builtin("paks", function(args)
  if args[1] == "confirm" then                             -- paks a .kmod wants to change (also on the Mods page)
    kmod.sync_mods()
    if args[2] then
      local ok, err = kmod.confirm_paks(args[2])
      if not ok then return "not installed: " .. tostring(err) end
      kmod.reload_mods()
      return "installed" .. (kmod.pak_restart and "; restart the game for the pak changes" or "")
    end
    local lines = {}
    for file, i in pairs(kmod.pak_pending) do
      lines[#lines + 1] = string.format("  %s: %s %s  writes [%s] removes [%s] conflicts [%s]", file, i.name, i.version,
        table.concat(i.write or {}, ", "), table.concat(i.remove or {}, ", "), table.concat(i.conflicts or {}, ", "))
    end
    return #lines > 0 and table.concat(lines, "\n") or "no package is waiting to change paks"
  end
  if args[1] == "off" or args[1] == "on" then
    if not args[2] then return "usage: paks " .. args[1] .. " <file name>" end
    kmod.pak_off(args[2], args[1] == "off")
    return args[2] .. (args[1] == "off" and " is switched off from the next start on (an already mounted pak stays until then)" or " will be mounted again at the next start")
  end
  local list = kmod.pak_status()
  if #list == 0 then return "no .pak files in kmod/paks" end
  local lines = {}
  for _, p in ipairs(list) do lines[#lines + 1] = string.format("  %-40s %s", p.name, p.status) end
  return table.concat(lines, "\n")
end, "list the pak files in kmod/paks with their status; 'paks off <file>' / 'paks on <file>' switch one off or on; 'paks confirm' handles pak updates of .kmod packages", "paks [off|on <file>] | paks confirm [<package.kmod>]")

local function setEnabled(id, on)
  local rec = id and findMod(id)
  if not rec then return "unknown mod '" .. tostring(id) .. "' (see: mods)" end
  kmod.set_enabled(id, on)
  kmod.reload_mods()
  return id .. (on and " enabled" or " disabled")
end
builtin("enable", function(args) return setEnabled(args[1], true) end, "enable a mod and reload", "enable <modid>")
builtin("disable", function(args) return setEnabled(args[1], false) end, "disable a mod and reload", "disable <modid>")
builtin("reload", function() kmod.reload_mods(); return "mods reloaded" end, "reload all mods")

builtin("lang", function(args)
  if not args[1] then return "language: " .. kmod.lang_current() .. " (setting: " .. kmod.state.language .. ")" end
  local tag = args[1]
  local ok = tag == "auto"
  for _, t in ipairs(kmod.language_tags) do if t == tag then ok = true end end
  if not ok then return "unknown language '" .. tag .. "' (auto, " .. table.concat(kmod.language_tags, ", ") .. ")" end
  kmod.set_language(tag)
  return "language set to " .. tag
end, "show or set the mod UI language", "lang [auto|en|zh-CN|...]")

builtin("settings", function(args)
  local rec = args[1] and findMod(args[1])
  if not rec then return "unknown mod '" .. tostring(args[1]) .. "' (see: mods)" end
  if not args[2] then
    local lines = {}
    for _, s in ipairs(rec.settings) do
      lines[#lines + 1] = string.format("  %-16s %-8s %s", s.id, s.type, tostring(kmod.settings_get(rec, s.id)))
    end
    return #lines > 0 and table.concat(lines, "\n") or (rec.id .. " declares no settings")
  end
  local s = rec.settingById and rec.settingById[args[2]]
  if not s then return "no setting '" .. args[2] .. "' in " .. rec.id end
  if args[3] == nil then return tostring(kmod.settings_get(rec, s.id)) end
  local raw, v = args[3], nil
  if s.type == "bool" then
    if raw == "true" or raw == "1" or raw == "on" then v = true elseif raw == "false" or raw == "0" or raw == "off" then v = false end
    if v == nil then return "expected true/false" end
  elseif s.type == "number" then
    v = tonumber(raw)
    if not v then return "expected a number" end
    if s.min and v < s.min then v = s.min end
    if s.max and v > s.max then v = s.max end
  elseif s.type == "choice" then
    for _, o in ipairs(s.options or {}) do if o == raw then v = o end end
    if v == nil then return "expected one of: " .. table.concat(s.options or {}, ", ") end
  else
    v = raw
    if s.maxlen and #v > s.maxlen then v = v:sub(1, s.maxlen) end
  end
  kmod.settings_set(rec, s.id, v)
  return rec.id .. "." .. s.id .. " = " .. tostring(v)
end, "list, read or change a mod's settings", "settings <modid> [key [value]]")

builtin("log", function(args)
  local n = math.max(1, math.min(tonumber(args[1]) or 20, 200))
  local f = io.open(kmod.paths.logs .. "/kmod.log", "rb")
  if not f then return "no log file" end
  local size = f:seek("end")
  f:seek("set", math.max(0, size - 65536))
  local data = f:read("a") or ""
  f:close()
  local lines = {}
  for l in data:gmatch("[^\r\n]+") do lines[#lines + 1] = l end
  local from = math.max(1, #lines - n + 1)
  return table.concat(lines, "\n", from, #lines)
end, "show the last lines of kmod.log", "log [count]")

-- the overlay (ImGui) shows this list: one line per mod, tab separated: id, title, version, enabled (1/0), status
function kmod.overlay_snapshot()
  local lines, tab, nl = {}, string.char(9), string.char(10)
  for _, rec in ipairs(kmod.refresh_mods() or {}) do
    local status = rec.status or ""
    if #rec.errors > 0 then status = rec.errors[1] end
    status = status:gsub("[%c]", " ")
    lines[#lines + 1] = table.concat({ rec.id, kmod.mod_title(rec), rec.version or "", rec.enabled and "1" or "0", status }, tab)
  end
  return table.concat(lines, nl)
end

builtin("dump", function(args)
  local what = args[1]
  if what == "objects" then local path, n = DumpAllObjects(args[2]); return n .. " objects -> " .. path end
  if what == "actors" then local path, n = DumpAllActors(args[2]); return n .. " actors -> " .. path end
  return "usage: dump objects|actors [file name]   (written into the logs folder)"
end, "write all objects / actors to a text file (like UE4SS's object dump)", "dump objects|actors [file]")

builtin("find", function(args)
  if not args[1] then return "usage: find <class> [limit]" end
  local list = FindAllOf(args[1]) or {}
  local limit = tonumber(args[2]) or 20
  local lines = { #list .. " object(s)" }
  for i = 1, math.min(#list, limit) do lines[#lines + 1] = "  " .. list[i]:GetPathName() end
  if #list > limit then lines[#lines + 1] = "  ... (" .. (#list - limit) .. " more)" end
  return table.concat(lines, "\n")
end, "list live objects of a class", "find <class> [limit]")

local function findClass(name)
  local c = FindObject("Class", name)
  if not c:IsValid() then return nil end
  return c
end

builtin("props", function(args)
  if not args[1] then return "usage: props <class>" end
  local c = findClass(args[1])
  if not c then return "class '" .. args[1] .. "' not found (use the name without U/A prefix; try: search " .. args[1] .. ")" end
  local lines = {}
  for _, v in pairs(c:GetProperties()) do
    lines[#lines + 1] = "  " .. tostring(v.name) .. " : " .. tostring(v.type) .. "  (" .. tostring(v.owner) .. ")"
  end
  table.sort(lines)
  return #lines .. " props\n" .. table.concat(lines, "\n")
end, "list the properties of a class", "props <class>")

builtin("funcs", function(args)
  if not args[1] then return "usage: funcs <class> [filter]" end
  local c = findClass(args[1])
  if not c then return "class '" .. args[1] .. "' not found (use the name without U/A prefix; try: search " .. args[1] .. ")" end
  local filter = args[2] and args[2]:lower()
  local lines = {}
  for _, f in ipairs(kmod.signatures(c)) do
    if not filter or f.name:lower():find(filter, 1, true) then
      lines[#lines + 1] = "  " .. f.owner .. "." .. f.name .. f.sig
    end
  end
  table.sort(lines)
  return #lines .. " funcs\n" .. table.concat(lines, "\n")
end, "list a class's functions with their parameters", "funcs <class> [filter]")

-- search <words...> [class|object|function|all] [limit]
builtin("search", function(args)
  local kinds = { class = "class", c = "class", object = "object", o = "object", ["function"] = "function", func = "function", f = "function", all = "all", a = "all" }
  local limit, kind = 20, "all"
  local words = {}
  for _, a in ipairs(args) do
    if kinds[a:lower()] and #words > 0 then kind = kinds[a:lower()]
    elseif a:match("^%d+$") and #words > 0 then limit = tonumber(a)
    else words[#words + 1] = a end
  end
  if #words == 0 then return "usage: search <words...> [class|object|function|all] [limit]" end
  local pattern = table.concat(words, " ")
  local out = {}
  local function section(k, title)
    local res, total = kmod.search(pattern, k, limit)
    table.sort(res, function(x, y) if #x.path ~= #y.path then return #x.path < #y.path end return x.path < y.path end)
    out[#out + 1] = title .. " (" .. total .. (total > #res and (", showing " .. #res) or "") .. ")"
    for _, r in ipairs(res) do out[#out + 1] = "  " .. r.path .. (k == "object" and ("  [" .. r.class .. "]") or "") end
  end
  if kind == "all" or kind == "class" then section("class", "classes") end
  if kind == "all" or kind == "object" then section("object", "objects") end
  if kind == "all" or kind == "function" then section("function", "functions") end
  return table.concat(out, "\n")
end, "search classes, objects and functions by path (all words must match, any case)", "search <words...> [class|object|function|all] [limit]")

-- ------------------------------------------------------------------ hook: print the arguments of every call
local hooks = kmod._cmd_hooks or {}          -- path -> { id, calls, max, params }
kmod._cmd_hooks = hooks
local DEFAULT_CALLS = 20

local function fmt(v, depth)
  depth = depth or 0
  local t = type(v)
  if t == "string" then
    local s = #v > 60 and (v:sub(1, 60) .. "...") or v
    return '"' .. s .. '"'
  elseif t == "number" or t == "boolean" or t == "nil" then return tostring(v)
  elseif t == "userdata" then
    local ok, s = pcall(function() if v:IsValid() then return v:GetPathName() end return "invalid" end)
    if ok then return "<" .. s .. ">" end
    return tostring(v)
  elseif t == "table" then
    if depth >= 1 then return "{...}" end
    local parts, n = {}, 0
    for k, x in pairs(v) do
      n = n + 1
      if n > 4 then parts[#parts + 1] = "..."; break end
      parts[#parts + 1] = tostring(k) .. "=" .. fmt(x, depth + 1)
    end
    return "{" .. table.concat(parts, ", ") .. "}"
  end
  return tostring(v)
end

local function selfName(self)
  local ok, s = pcall(function() return self:GetPathName() end)
  return ok and s or "?"
end

local function describe(params, p, outputs)
  local parts = {}
  for _, pi in ipairs(params) do
    local isOut = pi.out or pi.ret
    if isOut == outputs then
      local ok, v = pcall(function() return p[pi.name] end)
      parts[#parts + 1] = pi.name .. "=" .. (ok and fmt(v) or "?")
    end
  end
  return table.concat(parts, ", ")
end

local function unhook(path)
  local h = hooks[path]
  if not h then return false end
  UnregisterHook(h.id)
  hooks[path] = nil
  return true
end

local function SigOfParams(params)
  local parts = {}
  for _, pi in ipairs(params) do
    if not pi.ret then parts[#parts + 1] = (pi.out and "out " or "") .. pi.name .. ": " .. pi.type end
  end
  return "(" .. table.concat(parts, ", ") .. ")"
end

builtin("hook", function(args)
  local path = args[1]
  if not path then return "usage: hook <function path> [max calls (default " .. DEFAULT_CALLS .. ", 0 = unlimited)]" end
  if hooks[path] then return "already hooked (unhook " .. path .. " first)" end
  local max = args[2] and tonumber(args[2]) or DEFAULT_CALLS
  local params = kmod.func_params(path)
  if not params then return "function not found: " .. path .. " (try: search <word> function)" end
  local hasOut = false
  for _, pi in ipairs(params) do if pi.out or pi.ret then hasOut = true end end
  local h = { calls = 0, max = max, params = params }
  local function pre(self, p)
    h.calls = h.calls + 1
    print(string.format("[hook %d] %s  self=%s  (%s)", h.calls, path, selfName(self), describe(params, p, false)))
    if max > 0 and h.calls >= max and not hasOut then
      unhook(path); print("[hook] " .. path .. ": reached " .. max .. " calls, removed")
    end
  end
  local function post(self, p)
    if hasOut then
      print(string.format("[hook %d]   -> %s", h.calls, describe(params, p, true)))
      if max > 0 and h.calls >= max and hooks[path] then
        unhook(path); print("[hook] " .. path .. ": reached " .. max .. " calls, removed")
      end
    end
  end
  local ok, id = pcall(RegisterHook, path, pre, post)
  if not ok then return "cannot hook " .. path .. ": " .. tostring(id):match("^[^\n]*") end
  h.id = id
  hooks[path] = h
  return "hooked " .. path .. " " .. SigOfParams(params) .. (max > 0 and (" for " .. max .. " calls") or " (unlimited)")
end, "print the arguments of every call to a function", "hook <function path> [max calls]")

builtin("unhook", function(args)
  if args[1] == "all" or args[1] == nil then
    local n = 0
    for path in pairs(hooks) do unhook(path); n = n + 1 end
    return n .. " hook(s) removed"
  end
  return unhook(args[1]) and ("removed " .. args[1]) or ("not hooked: " .. args[1])
end, "remove one console hook, or all", "unhook [function path|all]")

builtin("hooks", function()
  local lines = {}
  for path, h in pairs(hooks) do lines[#lines + 1] = string.format("  %-60s %d call(s)%s", path, h.calls, h.max > 0 and (" / " .. h.max) or "") end
  table.sort(lines)
  return #lines > 0 and table.concat(lines, "\n") or "no console hooks"
end, "list active console hooks")

-- ------------------------------------------------------------------ typegen: type declarations for editors (LuaLS)
local API_STUB = [==[
---@meta
---@diagnostic disable: missing-fields
-- kmod loader API declarations for the Lua language server (LuaLS). Generated by the `typegen` command.

---@class KObject
---@field IsValid fun(self: KObject): boolean
---@field GetName fun(self: KObject): string
---@field GetFullName fun(self: KObject): string
---@field GetPathName fun(self: KObject): string
---@field GetClass fun(self: KObject): KObject
---@field GetOuter fun(self: KObject): KObject
---@field GetDefaultObject fun(self: KObject): KObject
---@field IsA fun(self: KObject, class: string|KObject): boolean
---@field GetPropertyValue fun(self: KObject, name: string): any
---@field SetPropertyValue fun(self: KObject, name: string, value: any)
---@field CallFunction fun(self: KObject, name: string, ...): any
---@field GetProperties fun(self: KObject): table[]
---@field GetFunctions fun(self: KObject): string[]
---@field GetAddress fun(self: KObject): integer

---@class HookParams
---@field [string] any

---@class ModInfo
---@field id string
---@field name string
---@field version string
---@field dir string
---@field data_dir string
---@field path fun(rel: string): string
---@type ModInfo
Mod = {}

---@class KLog
---@field debug fun(...)
---@field info fun(...)
---@field warn fun(...)
---@field error fun(...)
---@type KLog
Log = {}

---@class KSettings
---@field get fun(id: string): any
---@field set fun(id: string, value: any)
---@field on_change fun(fn: fun(id: string, value: any))
---@type KSettings
Settings = {}

---@class KLang
---@field t fun(key: string, vars?: table<string, any>): string
---@field current fun(): string
---@field on_change fun(fn: fun(tag: string))
---@type KLang
Lang = {}

---@class KConsoleInfo
---@field help? string
---@field usage? string

---@class CommandContext
---@field line string
---@field print fun(...)

---@alias CommandHandler fun(args: string[], ctx: CommandContext): string?

---@class KConsole
---@field register fun(name: string, handler: CommandHandler, info?: string|KConsoleInfo): boolean, string?
---@field unregister fun(name: string): boolean
---@field exec fun(line: string): boolean
---@field print fun(...)
---@type KConsole
Console = {}

---@class KmodInfo
---@field version string
---@field api integer
---@field flavor "dev"|"release"
---@field game string?
---@field game_version string?
---@field max_errors integer
---@field paths { files: string, root: string, mods: string, dev: string, logs: string }
---@field version_satisfies fun(have: string, cond: string): boolean
---@field print_tree fun(widget: KObject, depth?: integer)
---@type KmodInfo
kmod = {}

---@generic T
---@param className `T`
---@return T
function FindFirstOf(className) end

---@generic T
---@param className `T`
---@return T[]?
function FindAllOf(className) end

---@generic T
---@param className `T`
---@param name string
---@return T
function FindObject(className, name) end

---@param path string
---@return KObject
function StaticFindObject(path) end

---@param path string
---@return KObject?, string?
function LoadAsset(path) end

---@return KObject
function CreateInvalidObject() end

---@param path string
---@param pre fun(self: KObject, params: HookParams)
---@param post? fun(self: KObject, params: HookParams)
---@return integer id, integer id2
function RegisterHook(path, pre, post) end

---@param id integer
function UnregisterHook(id) end

---@param ms number
---@param fn fun()
function ExecuteWithDelay(ms, fn) end

---@param fn fun()
function ExecuteInGameThread(fn) end

---@param ms number
---@param fn fun(): boolean?
function LoopAsync(ms, fn) end

---@generic T
---@param className `T`
---@param fn fun(obj: T)
function NotifyOnNewObject(className, fn, fast) end

---@param ms number
---@param fn fun(): boolean?
function FastLoop(ms, fn) end

---@param fn fun()
function ExecuteAsync(fn) end

---@class KWorker
local KWorker = {}
---@param msg any nil/boolean/number/string/table only
function KWorker:post(msg) end
---@param fn fun(msg: any)
---@return KWorker
function KWorker:onMessage(fn) return self end
---@return boolean
function KWorker:alive() end
function KWorker:close() end

---@param file string script path relative to the mod folder
---@return KWorker
function Worker(file) end

---@class KEvents
Events = {}
---@param name string
---@param fn fun(...)
---@return table handle
function Events.on(name, fn) end
---@param handle table
function Events.off(handle) end
---@param name string
---@return integer handlers_called
function Events.emit(name, ...) end

---@class KStore
Store = {}
---@param key string
---@param default any
---@return any
function Store.get(key, default) end
---@param key string
---@param value any
function Store.set(key, value) end

---@class KWindow
local KWindow = {}
---@param text string
---@return { set_text: fun(self, text: string) }
function KWindow:label(text) end
---@param text string
---@param fn fun()
function KWindow:button(text, fn) end
---@param label string
---@param value boolean
---@param fn fun(on: boolean)
function KWindow:toggle(label, value, fn) end
---@param label string
---@param min number
---@param max number
---@param value number
---@param fn fun(value: number)
---@param step? number
function KWindow:slider(label, min, max, value, fn, step) end
---@param label string
---@param value string
---@param fn fun(text: string)
function KWindow:input(label, value, fn) end
---@param label string
---@param options string[]
---@param selected string
---@param fn fun(option: string)
function KWindow:choice(label, options, selected, fn) end
---@param height? number
function KWindow:spacer(height) end
function KWindow:clear() end
function KWindow:show() end
function KWindow:hide() end
function KWindow:toggle_visible() end
---@return boolean
function KWindow:is_open() end
function KWindow:close() end

---@class KUI
UI = {}
---@param opts { title?: string, width?: number, max_height?: number, show?: boolean, on_close?: fun() }
---@return KWindow
function UI.window(opts) end
---@param text string
---@param seconds? number
function UI.toast(text, seconds) end

---@class KHttpResponse
---@field status integer 0 when the request did not get an answer
---@field body string
---@field headers table<string, string>
---@field error? string

---@class KHttp
Http = {}
---@param url string
---@param opts? { headers?: table<string,string>, timeout?: integer, max_bytes?: integer }|fun(resp: KHttpResponse)
---@param cb? fun(resp: KHttpResponse)
---@return integer id
function Http.get(url, opts, cb) end
---@param url string
---@param body string|table
---@param opts? table|fun(resp: KHttpResponse)
---@param cb? fun(resp: KHttpResponse)
---@return integer id
function Http.post(url, body, opts, cb) end
---@param opts { method?: string, url: string, body?: string, headers?: table<string,string>, timeout?: integer, max_bytes?: integer }
---@param cb fun(resp: KHttpResponse)
---@return integer id
function Http.request(opts, cb) end
---@param resp KHttpResponse
---@return any
---@param url string
---@param path string
---@param opts? table|fun(resp: KHttpResponse)
---@param cb? fun(resp: KHttpResponse)
---@param progress? fun(bytes: number, total: number)
---@return integer id
function Http.download(url, path, opts, cb, progress) end
function Http.json(resp) end

---@class KConnection
local KConnection = {}
---@param data string
---@return boolean queued
function KConnection:send(data) end
---@param data string
---@return boolean queued
function KConnection:send_binary(data) end
function KConnection:close() end
---@return boolean
function KConnection:is_open() end

---@class KWebSocketHandlers
---@field headers? table<string,string>
---@field timeout? integer
---@field on_open? fun()
---@field on_message? fun(data: string, is_binary: boolean)
---@field on_close? fun(code: integer, reason: string)
---@field on_error? fun(message: string)

---@class KWebSocket
WebSocket = {}
---@param url string ws:// or wss://
---@param handlers KWebSocketHandlers
---@return KConnection
function WebSocket.connect(url, handlers) end

---@class KSocketHandlers
---@field tls? boolean
---@field timeout? integer
---@field on_open? fun()
---@field on_data? fun(data: string)
---@field on_close? fun(code: integer, reason: string)
---@field on_error? fun(message: string)

---@class KSocket
Socket = {}
---@param host string
---@param port integer
---@param handlers KSocketHandlers
---@return KConnection
function Socket.tcp(host, port, handlers) end
---@param host string
---@param port integer
---@param handlers KSocketHandlers
---@return KConnection
function Socket.udp(host, port, handlers) end

---@class KGameMessage
---@field channel string
---@field sender string
---@field message string
---@field context string

---@class KNet
Net = {}
---@param fn fun(msg: KGameMessage): table?
function Net.on_message(fn) end
---@param fn fun(state: "open"|"closed"|"failure", detail?: string)
function Net.on_state(fn) end
---@param channel string
---@param receiver string
---@param message string
---@param context? string
function Net.send(channel, receiver, message, context) end
---@return boolean
function Net.is_connected() end
---@return string
function Net.url() end
---@return string
function Net.redirect_status() end
]==]

local LUARC = [==[
{
  "runtime": { "version": "Lua 5.4" },
  "workspace": { "library": [ "./types" ], "checkThirdParty": false },
  "diagnostics": { "globals": [ "Mod", "Log", "Settings", "Lang", "Console", "kmod" ] }
}
]==]

local function writeFile(path, data)
  local f = io.open(path, "wb")
  if not f then return false end
  f:write(data); f:close()
  return true
end

builtin("typegen", function(args)
  local dir = kmod.paths.root .. "/types"
  local t0 = os.clock()
  local files, classes, structs, enums, funcs = kmod.dump_types(dir, args)
  local ok1 = writeFile(dir .. "/kmod_api.lua", API_STUB)
  writeFile(dir .. "/.luarc.json", LUARC)
  return string.format("wrote %d type file(s) to %s\n  %d classes, %d structs, %d enums, %d functions (%.1f s)%s\nCopy that folder next to your mod (with its .luarc.json) and open it in VS Code with the Lua extension.",
    files + (ok1 and 1 or 0), dir, classes, structs, enums, funcs, os.clock() - t0,
    #args > 0 and ("\n  filter: " .. table.concat(args, ", ")) or "")
end, "write type declarations for editors (LuaLS) to <root>/types", "typegen [package filter...]   e.g. typegen Engine kards")
