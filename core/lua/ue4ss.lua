-- kmod: the part of the UE4SS Lua API that is written in Lua on top of the core (built-in script; per-mod functions are added to
-- a mod's environment by kmod.ue4ss_api, called by the mod loader). Delayed actions, lifecycle hooks, console hook, mod management.
-- Where the engine function UE4SS hooks is not a reflected UFunction (BeginPlay is, InitGameState and LoadMap are not) the
-- callback is driven by an equivalent observation; the differences are listed in docs (lua-api/ue4ss-compat).

local FRAME_MS = 1000 / 60

function kmod.ue4ss_api(rec, env)
  local now = kmod.now
  local function warn(msg) kmod.log("warn", rec.id, msg) end
  local function safe(fn, ...)
    local ok, err = pcall(fn, ...)
    if not ok then
      kmod.log("error", rec.id, "callback failed: " .. tostring(err))
      if kmod._mod_error then kmod._mod_error(rec.id, tostring(err)) end
    end
  end

  -- ------------------------------------------------------------ delayed actions (handles)
  local actions, nextHandle, ticking = {}, 1, false
  local function tick()
    local t = now()
    local due = {}
    for h, a in pairs(actions) do if not a.paused and t >= a.due then due[#due + 1] = h end end
    for _, h in ipairs(due) do
      local a = actions[h]
      if a and not a.paused then
        if a.loop then a.due = t + a.ms / 1000 else actions[h] = nil end
        safe(a.fn)
      end
    end
    if next(actions) == nil then ticking = false; return true end       -- nothing left: stop the loop
    return false
  end
  local function start()
    if ticking then return end
    ticking = true
    env.FastLoop(4, tick)
  end
  local function create(ms, fn, loop, handle, frames)
    if handle == nil then handle = nextHandle; nextHandle = nextHandle + 1 end
    actions[handle] = { fn = fn, ms = ms, loop = loop, due = now() + ms / 1000, paused = false, frames = frames }
    start()
    return handle
  end
  env.EngineTickAvailable = true                               -- frame-based delays are approximated at 60 frames per second
  env.MakeActionHandle = function() local h = nextHandle; nextHandle = nextHandle + 1; return h end
  env.ExecuteInGameThreadWithDelay = function(a, b, c)
    if type(b) == "function" then return create(a, b, false) end
    if actions[a] then return a end                            -- (handle, delay, fn): only when not already running
    return create(b, c, false, a)
  end
  env.RetriggerableExecuteInGameThreadWithDelay = function(h, ms, fn) create(ms, fn, false, h) end
  env.LoopInGameThreadWithDelay = function(ms, fn) return create(ms, fn, true) end
  env.ExecuteInGameThreadAfterFrames = function(frames, fn) return create(frames * FRAME_MS, fn, false, nil, frames) end
  env.LoopInGameThreadAfterFrames = function(frames, fn) return create(frames * FRAME_MS, fn, true, nil, frames) end
  env.CancelDelayedAction = function(h) if actions[h] then actions[h] = nil; return true end return false end
  env.PauseDelayedAction = function(h)
    local a = actions[h]
    if not a or a.paused then return false end
    a.paused = true; a.remaining = math.max(0, a.due - now()); return true
  end
  env.UnpauseDelayedAction = function(h)
    local a = actions[h]
    if not a or not a.paused then return false end
    a.paused = false; a.due = now() + a.remaining; return true
  end
  env.ResetDelayedActionTimer = function(h)
    local a = actions[h]
    if not a then return false end
    if a.paused then a.remaining = a.ms / 1000 else a.due = now() + a.ms / 1000 end
    return true
  end
  env.SetDelayedActionTimer = function(h, ms)
    local a = actions[h]
    if not a then return false end
    a.ms = ms; a.frames = nil
    return env.ResetDelayedActionTimer(h)
  end
  env.ClearAllDelayedActions = function() local n = 0; for h in pairs(actions) do actions[h] = nil; n = n + 1 end return n end
  env.IsValidDelayedActionHandle = function(h) return actions[h] ~= nil end
  env.IsDelayedActionActive = function(h) local a = actions[h]; return a ~= nil and not a.paused end
  env.IsDelayedActionPaused = function(h) local a = actions[h]; return a ~= nil and a.paused end
  local function units(a, ms) if a.frames then return ms / FRAME_MS end return ms end
  env.GetDelayedActionRate = function(h) local a = actions[h]; if not a then return -1 end return a.frames or a.ms end
  local function remainingMs(a) return math.max(0, (a.paused and a.remaining or (a.due - now())) * 1000) end
  env.GetDelayedActionTimeRemaining = function(h) local a = actions[h]; if not a then return -1 end return units(a, remainingMs(a)) end
  env.GetDelayedActionTimeElapsed = function(h) local a = actions[h]; if not a then return -1 end return units(a, a.ms - remainingMs(a)) end

  -- ------------------------------------------------------------ functions hooked by NAME in every class that has one
  -- (a Blueprint class overriding ReceiveBeginPlay has its own UFunction, so each is hooked; new classes are picked up later)
  local function hookAll(fname, pre, post)
    local seen = {}
    local function scan()
      for _, f in ipairs(FindObjects(0, "Function", fname, 0, 0, true) or {}) do
        local a = f:GetAddress()
        if not seen[a] then
          seen[a] = true
          pcall(env.RegisterHook, f:GetFullName(), pre, post)
        end
      end
    end
    scan()
    env.LoopAsync(10000, function() scan(); return false end)
  end
  -- the same actor passes through a function and its Super: report it once
  local function dedupe(cb)
    local last, n = {}, 0
    return function(self, ...)
      local a = self:GetAddress()
      local t = now()
      if last[a] and t - last[a] < 0.1 then return end
      last[a] = t; n = n + 1
      if n > 4000 then last, n = {}, 0 end
      cb(self, ...)
    end
  end

  env.RegisterBeginPlayPreHook = function(cb) hookAll("ReceiveBeginPlay", dedupe(function(self) safe(cb, self) end), nil) end
  env.RegisterBeginPlayPostHook = function(cb) hookAll("ReceiveBeginPlay", function() end, dedupe(function(self) safe(cb, self) end)) end
  env.RegisterEndPlayPreHook = function(cb) hookAll("ReceiveEndPlay", dedupe(function(self) safe(cb, self) end), nil) end
  env.RegisterEndPlayPostHook = function(cb) hookAll("ReceiveEndPlay", function() end, dedupe(function(self) safe(cb, self) end)) end

  -- RegisterCustomEvent(name, fn): every Blueprint function / event with that name
  env.RegisterCustomEvent = function(name, cb)
    hookAll(name, function(self, p) safe(cb, self, p) end, nil)
  end

  -- InitGameState: not a reflected function; the callbacks run when a new GameState object appears (post: shortly after)
  env.RegisterInitGameStatePreHook = function(cb) env.NotifyOnNewObject("GameStateBase", function(o) safe(cb, o) end) end
  env.RegisterInitGameStatePostHook = function(cb)
    env.NotifyOnNewObject("GameStateBase", function(o) env.ExecuteWithDelay(100, function() if o:IsValid() then safe(cb, o) end end) end)
  end

  -- LoadMap: not a reflected function; the callbacks run when the game viewport's world changes (cb(engine, nil, worldName))
  local loadMapPre, loadMapPost, watching, lastWorld = {}, {}, false, nil
  local function worldName()
    local e = FindFirstOf("GameEngine")
    if not e or not e:IsValid() then return nil end
    local vp = e.GameViewport
    if not vp or not vp:IsValid() then return nil end
    local w = vp.World
    return (w and w:IsValid()) and w:GetFullName() or nil, e
  end
  local function watchWorld()
    if watching then return end
    watching = true
    lastWorld = worldName()
    env.LoopAsync(250, function()
      local name, e = worldName()
      if name and name ~= lastWorld then
        lastWorld = name
        for _, cb in ipairs(loadMapPre) do safe(cb, e, nil, name) end
        env.ExecuteWithDelay(300, function() for _, cb in ipairs(loadMapPost) do safe(cb, e, nil, name) end end)
      end
      return false
    end)
  end
  env.RegisterLoadMapPreHook = function(cb) loadMapPre[#loadMapPre + 1] = cb; watchWorld() end
  env.RegisterLoadMapPostHook = function(cb) loadMapPost[#loadMapPost + 1] = cb; watchWorld() end

  -- ProcessConsoleExec: the console command functions the game reaches by reflection; cb(context, fullCommand, parts).
  -- (The callback's return value cannot cancel or change the command.)
  local function consoleHook(post)
    return function(cb)
      local function handler(self, p)
        local cmd = tostring(p.Command or "")
        local parts = {}
        for w in cmd:gmatch("%S+") do parts[#parts + 1] = w end
        safe(cb, self, cmd, parts)
      end
      pcall(env.RegisterHook, "PlayerController:ConsoleCommand", post and function() end or handler, post and handler or nil)
      pcall(env.RegisterHook, "KismetSystemLibrary:ExecuteConsoleCommand", post and function() end or handler, post and handler or nil)
    end
  end
  env.RegisterProcessConsoleExecPreHook = consoleHook(false)
  env.RegisterProcessConsoleExecPostHook = consoleHook(true)
  local function unsupported(name)
    env[name] = function() warn(name .. " is not available in kmod (it hooks an engine C++ function that is not reflected); ignored") end
  end
  for _, n in ipairs({ "RegisterULocalPlayerExecPreHook", "RegisterULocalPlayerExecPostHook",
                       "RegisterCallFunctionByNameWithArgumentsPreHook", "RegisterCallFunctionByNameWithArgumentsPostHook",
                       "GenerateSDK", "GenerateUHTCompatibleHeaders", "DumpUSMAP", "DumpJMAP", "DumpStaticMeshes" }) do unsupported(n) end
  env.GenerateLuaTypes = function(...) return kmod.dump_types(...) end

  -- ------------------------------------------------------------ mod management (kmod reloads every mod; there is no per-mod restart)
  local function reloadLater() _G.ExecuteWithDelay(100, function() kmod.reload_mods() end) end
  local function findRec(name)
    for _, r in ipairs(kmod.mods or {}) do if r.id == name or r.name == name then return r end end
  end
  env.RestartCurrentMod = reloadLater
  env.RestartMod = function(name) if findRec(name) then reloadLater() else warn("RestartMod: no mod called " .. tostring(name)) end end
  env.UninstallCurrentMod = function() kmod.set_enabled(rec.id, false); reloadLater() end
  env.UninstallMod = function(name)
    local r = findRec(name)
    if r then kmod.set_enabled(r.id, false); reloadLater() else warn("UninstallMod: no mod called " .. tostring(name)) end
  end

  env.Print = env.print
  env.PrintLn = env.print
  env.PropertyTypes = setmetatable({}, { __index = function(_, k) return k end })     -- PropertyTypes.IntProperty == "IntProperty"
end
