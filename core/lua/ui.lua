-- kmod UI toolkit (built-in script). Gives every mod an `UI` table (see modloader.lua) to build windows out of the game's own
-- controls: UI.window{...} with :label / :button / :toggle / :slider / :input / :choice, and UI.toast(text).
--
-- A window is a plain UserWidget (UMG's base class, created through WidgetBlueprintLibrary:Create) whose widget tree is built
-- here: a CanvasPanel root, a Border panel centred on screen, a SizeBox that limits the width/height, and a ScrollBox for the
-- controls. The controls are the game's own widget blueprints (the same ones the Mods page uses), so they look native.
-- Texts are set a moment after the controls exist (a freshly created game widget resets its text when it is constructed).

local function log(...) kmod.log("info", "ui", ...) end

local PATH = {
  click = "WBP_NUI_MasterTextButton_C:BndEvt__Button_0_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature",
  close = "W_CloseButton_C:BndEvt__W_CloseButton_CloseButton_K2Node_ComponentBoundEvent_0_OnButtonClickedEvent__DelegateSignature",
  check = "WBP_NUI_MasterCheckbox_C:BndEvt__WBP_NUI_Checkbox_NUI_CheckBox_K2Node_ComponentBoundEvent_0_OnCheckBoxComponentStateChanged__DelegateSignature",
  slide = "WBP_NUI_MasterSlider_C:BndEvt__WBP_NUI_MasterSlider_Slider_K2Node_ComponentBoundEvent_3_OnFloatValueChangedEvent__DelegateSignature",
  text  = "W_EditableTextBox_C:BndEvt__W_EditableTextBox_EditableTextBox_K2Node_ComponentBoundEvent_1_OnEditableTextBoxCommittedEvent__DelegateSignature",
  drop  = "WBP_NUI_MasterDropdown_C:BndEvt__WBP_NUI_MasterDropdown_MasterDropdown_ComboBox_K2Node_ComponentBoundEvent_4_OnSelectionChanged__DelegateSignature",
}
local Z_ORDER = 9000
local H = {}
kmod._uih = H

local ctrls = {}                 -- widget address -> control record
kmod._ui_windows = kmod._ui_windows or {}
local toast = nil

local function umg(name) return StaticFindObject("/Script/UMG." .. name) end
local function gclass(name) return FindObject("WidgetBlueprintGeneratedClass", name) end
local function lib() return StaticFindObject("/Script/UMG.Default__WidgetBlueprintLibrary") end
local function gp() return StaticFindObject("/Script/Engine.Default__GameplayStatics") end

-- run a mod's callback: errors go to the log and count against the mod; a crash inside is blamed on the mod
local function callMod(owner, what, fn, ...)
  if type(fn) ~= "function" then return end
  kmod.set_activity("mod", owner, "ui")
  local ok, err = pcall(fn, ...)
  kmod.set_activity()
  if not ok then
    kmod.log("error", owner, "UI " .. what .. " callback failed: " .. tostring(err))
    if kmod._mod_error then kmod._mod_error(owner, tostring(err)) end
  end
end

-- ------------------------------------------------------------------ the shared tree builder
local function newHost()
  local pc = FindFirstOf("PlayerController")
  if not (pc and pc:IsValid()) then error("UI: the game is not ready yet (no PlayerController)", 3) end
  local l = lib()
  local uw = l:Create(pc, umg("UserWidget"), pc)
  if not (uw and uw:IsValid()) then error("UI: could not create a widget", 3) end
  return { pc = pc, lib = l, gp = gp(), uw = uw, tree = uw.WidgetTree }
end

local function spawn(host, cls) return host.gp:SpawnObject(umg(cls), host.tree) end
local function create(host, name) return host.lib:Create(host.uw, gclass(name), host.pc) end

-- A canvas root with one bordered panel; returns host, border, canvas slot.
local function buildPanel(host, anchor, alignment, pos, pad, color)
  local canvas = spawn(host, "CanvasPanel")
  host.tree.RootWidget = canvas
  local border = spawn(host, "Border")
  local slot = canvas:AddChild(border)
  slot:SetAnchors({ Minimum = anchor, Maximum = anchor })
  slot:SetAlignment(alignment)
  slot:SetAutoSize(true)
  slot:SetPosition(pos)
  border:SetBrushColor(color or { R = 0.04, G = 0.04, B = 0.04, A = 0.93 })
  border:SetPadding(pad)
  return border
end

-- ------------------------------------------------------------------ Window
local Window = {}
Window.__index = Window

local function flush(win)
  win._flushQueued = false
  if not win.host or not win.host.uw:IsValid() then return end
  local q = win._pending
  win._pending = {}
  for _, fn in ipairs(q) do
    local ok, err = pcall(fn)
    if not ok then log("could not set up a control: " .. tostring(err)) end
  end
end

local function later(win, fn)             -- run fn once the controls have been constructed
  win._pending[#win._pending + 1] = fn
  if not win._flushQueued then
    win._flushQueued = true
    ExecuteWithDelay(120, function() flush(win) end)
  end
end

local function addChild(win, widget)
  local slot = win.body:AddChild(widget)
  pcall(function() slot:SetPadding({ Left = 0, Top = 7, Right = 0, Bottom = 7 }) end)
  return slot
end

-- a game settings row: label on the left, the control on the right
local function addRow(win, label, control)
  local r = create(win.host, "W_OptionsEntryBase_C")
  addChild(win, r)
  later(win, function()
    r.OptionNameText:SetText(label or "")
    pcall(function() r.HelpButtonSizeBox:SetVisibility(1) end)
  end)
  if control then r.ContentSlot:SetContent(control) end
  return r
end

local function track(win, widget, rec)
  rec.win = win
  ctrls[widget:GetAddress()] = rec
  win._controls[#win._controls + 1] = widget
  return rec
end

function Window:alive() return self.host ~= nil and self.host.uw:IsValid() end

function Window:label(text)
  local w = create(self.host, "WBP_NUI_MasterTextBlock_C")
  addChild(self, w)
  later(self, function() w:SetText(text or "") end)
  local c = { set_text = function(_, t) pcall(function() w:SetText(t or "") end) end }
  return c
end

function Window:button(text, fn)
  local w = create(self.host, "WBP_NUI_MasterTextButton_C")
  addChild(self, w)
  later(self, function() w:SetText(text or "") end)
  track(self, w, { kind = "click", cb = fn })
  return { set_text = function(_, t) pcall(function() w:SetText(t or "") end) end }
end

function Window:toggle(label, value, fn)
  local w = create(self.host, "WBP_NUI_MasterCheckbox_C")
  addRow(self, label, w)
  later(self, function() self.updating = true; w:SetIsChecked(value and true or false); self.updating = false end)
  track(self, w, { kind = "check", cb = fn })
  return {
    get = function() return w:GetIsChecked() end,
    set = function(_, v) self.updating = true; pcall(function() w:SetIsChecked(v and true or false) end); self.updating = false end,
  }
end

function Window:slider(label, min, max, value, fn, step)
  min, max = min or 0, max or 1
  local w = create(self.host, "WBP_NUI_MasterSlider_C")
  local row = addRow(self, label, w)
  local rec = { kind = "slide", cb = fn, label = label, row = row }
  later(self, function()
    self.updating = true
    local sl = w.Slider
    sl:SetMinValue(min); sl:SetMaxValue(max); sl:SetStepSize(step or (max - min) / 100)
    w.MinValue = min; w.MaxValue = max; w.StepSize = step or (max - min) / 100; w.DefaultValue = value or min
    sl:SetValue(value or min)
    self.updating = false
  end)
  track(self, w, rec)
  return {
    get = function() return w.Slider:GetValue() end,
    set = function(_, v) self.updating = true; pcall(function() w.Slider:SetValue(v) end); self.updating = false end,
  }
end

function Window:input(label, value, fn)
  local w = create(self.host, "W_EditableTextBox_C")
  addRow(self, label, w)
  later(self, function() pcall(function() w.EditableTextBox:SetText(value or "") end) end)
  track(self, w, { kind = "text", cb = fn })
  return {
    get = function() local ok, v = pcall(function() return w.EditableTextBox:GetText() end); return ok and tostring(v) or "" end,
    set = function(_, v) pcall(function() w.EditableTextBox:SetText(v or "") end) end,
  }
end

function Window:choice(label, options, selected, fn)
  local w = create(self.host, "WBP_NUI_MasterDropdown_C")
  addRow(self, label, w)
  later(self, function()
    self.updating = true
    w:ClearOptions()
    local sel = 0
    for i, o in ipairs(options) do w:AddOption(tostring(o)); if o == selected then sel = i - 1 end end
    w:SetSelectedIndex(sel)
    self.updating = false
  end)
  track(self, w, { kind = "drop", cb = fn, options = options })
  return { get = function() return options[w:GetSelectedIndex() + 1] end }
end

function Window:spacer(height)
  local sb = spawn(self.host, "SizeBox")
  sb:SetHeightOverride(height or 20)
  self.body:AddChild(sb)
end

function Window:clear()
  for _, w in ipairs(self._controls) do pcall(function() ctrls[w:GetAddress()] = nil end) end
  self._controls = {}
  self.body:ClearChildren()
end

function Window:show()
  if not self:alive() then error("UI: the window was closed", 2) end
  self.host.uw:SetVisibility(0)
  self.open = true
end

function Window:hide()
  if not self:alive() then return end
  self.host.uw:SetVisibility(1)
  self.open = false
end

function Window:toggle_visible() if self.open then self:hide() else self:show() end end
function Window:is_open() return self:alive() and self.open == true end

function Window:close()
  if self.host and self.host.uw:IsValid() then pcall(function() self.host.uw:RemoveFromParent() end) end
  if self._close then pcall(function() ctrls[self._close:GetAddress()] = nil end) end
  for _, w in ipairs(self._controls) do pcall(function() ctrls[w:GetAddress()] = nil end) end
  self._controls = {}
  self.host, self.open = nil, false
  local list = kmod._ui_windows[self.owner]
  if list then for i, x in ipairs(list) do if x == self then table.remove(list, i); break end end end
end

-- UI.window{ title = "...", width = 900, max_height = 640, show = true, on_close = fn }
function kmod.ui_window(owner, opts)
  opts = opts or {}
  local host = newHost()
  local win = setmetatable({ owner = owner, host = host, _pending = {}, _controls = {}, open = false }, Window)
  local border = buildPanel(host, { X = 0.5, Y = 0.5 }, { X = 0.5, Y = 0.5 }, { X = 0, Y = 0 }, { Left = 34, Top = 26, Right = 34, Bottom = 30 })
  local sz = spawn(host, "SizeBox")
  border:SetContent(sz)
  sz:SetWidthOverride(opts.width or 900)
  local vb = spawn(host, "VerticalBox")
  sz:SetContent(vb)
  -- header: title + close button
  local head = spawn(host, "HorizontalBox")
  vb:AddChild(head)
  local title = create(host, "WBP_NUI_MasterTextBlock_C")
  local tslot = head:AddChild(title)
  pcall(function() tslot:SetSize({ Value = 1.0, SizeRule = 1 }); tslot:SetVerticalAlignment(2) end)
  -- the game's own close button (the worn "x" icon at the top right of its screens)
  local closeBtn = create(host, "W_CloseButton_C")
  local cbox = spawn(host, "SizeBox")
  cbox:SetWidthOverride(84)
  cbox:SetHeightOverride(84)
  cbox:SetContent(closeBtn)
  local cslot = head:AddChild(cbox)
  pcall(function() cslot:SetVerticalAlignment(2); cslot:SetHorizontalAlignment(2) end)
  later(win, function() title:SetText(opts.title or "") end)
  track(win, closeBtn, { kind = "close", cb = function()
    win:hide()
    callMod(owner, "on_close", opts.on_close)
  end })
  win._controls[#win._controls] = nil       -- the close button is not a content control: win:clear() must not unregister it (the X would stop working)
  win._close = closeBtn
  -- body: a scroll box limited in height
  local limit = spawn(host, "SizeBox")
  local lslot = vb:AddChild(limit)
  pcall(function() lslot:SetPadding({ Left = 0, Top = 14, Right = 0, Bottom = 0 }) end)
  limit:SetMaxDesiredHeight(opts.max_height or 640)
  win.body = spawn(host, "ScrollBox")
  limit:SetContent(win.body)
  host.uw:AddToViewport(Z_ORDER)
  host.uw:SetVisibility(1)
  kmod._ui_windows[owner] = kmod._ui_windows[owner] or {}
  table.insert(kmod._ui_windows[owner], win)
  if opts.show ~= false then win:show() end
  return win
end

-- ------------------------------------------------------------------ toast
-- UI.toast("text", seconds)   one small message at the top of the screen; a new one replaces the old one
function kmod.ui_toast(owner, text, seconds)
  if toast and toast.uw and toast.uw:IsValid() then pcall(function() toast.uw:RemoveFromParent() end) end
  local host = newHost()
  local border = buildPanel(host, { X = 0.5, Y = 0.0 }, { X = 0.5, Y = 0.0 }, { X = 0, Y = 70 }, { Left = 30, Top = 14, Right = 30, Bottom = 14 },
    { R = 0.04, G = 0.04, B = 0.04, A = 0.88 })
  local t = create(host, "WBP_NUI_MasterTextBlock_C")
  local tbox = spawn(host, "SizeBox")                                -- a fixed width, or the text block wraps after a few letters
  local chars = utf8.len(text or "") or #(text or "")
  tbox:SetWidthOverride(math.max(320, math.min(1500, chars * 26 + 40)))
  tbox:SetContent(t)
  border:SetContent(tbox)
  host.uw:AddToViewport(Z_ORDER + 5)
  pcall(function() host.uw:SetVisibility(4) end)                   -- SelfHitTestInvisible: never blocks touches
  toast = host
  local mine = host
  ExecuteWithDelay(120, function() if mine.uw:IsValid() then pcall(function() t:SetText(text or "") end) end end)
  ExecuteWithDelay(math.floor((seconds or 3) * 1000) + 120, function()
    if mine.uw:IsValid() then pcall(function() mine.uw:RemoveFromParent() end) end
    if toast == mine then toast = nil end
  end)
end

-- ------------------------------------------------------------------ Web: a page of HTML / CSS / JS in the game's browser widget
-- (size limits: min_width, min_height, max_width, max_height)
-- UI.web{ html = "<...>" | dir = "ui" (+ entry = "index.html"), width = 860, height = 520, design_width = 860, position = "center", fullscreen = false,
--         on_message = fn(msg), on_ready = fn(), on_close = fn() }  ->  { send(tbl), close(), show(), hide(), is_open(), set_size(w, h), set_position(x, y) }
-- The page talks to Lua with kmod.send(obj) (JS) and receives with kmod.onmessage = function (obj) {...}; see docs/lua-api/mod-env/web.md.
local WEB_CLASS = "/Script/WebBrowserWidget.WebBrowser"
kmod._web_windows = kmod._web_windows or {}

local MIME = { png = "image/png", jpg = "image/jpeg", jpeg = "image/jpeg", gif = "image/gif", webp = "image/webp", svg = "image/svg+xml",
  css = "text/css", js = "application/javascript", json = "application/json", woff = "font/woff", woff2 = "font/woff2", ttf = "font/ttf",
  otf = "font/otf", ico = "image/x-icon", bmp = "image/bmp" }
local MAX_PAGE = 12 * 1024 * 1024

local function textOf(v)
  if type(v) == "string" then return v end
  local ok, s = pcall(function() return v:ToString() end)
  return ok and tostring(s) or tostring(v)
end

-- the JS every page gets: viewport, no zoom, the kmod.* helpers, and the channel to Lua (document.title + URL hash)
local WEB_PRELUDE = [==[
<meta name="viewport" content="width=@DESIGN@, user-scalable=no">
<style>html,body{touch-action:pan-x pan-y;overscroll-behavior:none;-webkit-user-select:none;user-select:none;-webkit-tap-highlight-color:transparent}input,textarea,[contenteditable]{-webkit-user-select:text;user-select:text}</style>
<script>
(function () {
  var DESIGN = @DESIGN@, seq = 0;
  /* the browser widget lays the page out in real pixels: on the desktop, scale the page so DESIGN CSS pixels fill the width (phones do this with the viewport above) */
  var scaled = false, desktop = !/Android/i.test(navigator.userAgent);
  if (desktop) {
    var fit = function () { if (!scaled && innerWidth > 200) document.documentElement.style.zoom = String(innerWidth / DESIGN); };
    fit(); addEventListener('resize', fit);
  }
  /* no zoom for the player */
  addEventListener('wheel', function (e) { if (e.ctrlKey) e.preventDefault(); }, { passive: false });
  addEventListener('keydown', function (e) { if ((e.ctrlKey || e.metaKey) && ['+', '-', '=', '0', '_'].indexOf(e.key) >= 0) e.preventDefault(); });
  addEventListener('contextmenu', function (e) { e.preventDefault(); });
  ['gesturestart', 'gesturechange', 'gestureend'].forEach(function (g) { addEventListener(g, function (e) { e.preventDefault(); }); });
  function post(o) {
    var s = 'kmod:' + (++seq) + ':' + JSON.stringify(o);
    document.title = s;
    try { history.replaceState(null, '', '#' + encodeURIComponent(s)); } catch (e) { location.hash = encodeURIComponent(s); }
  }
  /* UI units per screenX / screenY unit: pointer deltas for dragging and resizing (the page is scaled to fit on phones, zoomed on the desktop) */
  function unit() {
    var z = parseFloat(document.documentElement.style.zoom) || 1;
    var s = (window.visualViewport && window.visualViewport.scale) || 1;
    return 1 / (z * s);
  }
  /* where the window is and how big (UI units); the host keeps this up to date */
  var state = { x: 0, y: 0, w: DESIGN, h: 0 };
  /* Dragging. Desktop: the page only says "start" and "end", the host follows the mouse itself every frame (a page inside a moving window cannot measure the
     pointer). Phones: the host cannot see the finger while the browser has it, so the page measures it (screenX) and posts the new numbers once per frame;
     the "start" / "end" messages make the host read the page's messages every frame instead of 25 times a second. */
  function dragger(el, mode) {
    el.style.touchAction = 'none';
    el.addEventListener('pointerdown', function (e) {
      if (e.button) return;
      if (e.target && e.target.hasAttribute && e.target.hasAttribute('data-nodrag')) return;
      el.setPointerCapture(e.pointerId);
      var s0 = { x: e.screenX, y: e.screenY, st: { x: state.x, y: state.y, w: state.w, h: state.h } };
      var raf = 0, last = null;
      var send = function () {
        raf = 0;
        var u = unit(), dx = (last.screenX - s0.x) * u, dy = (last.screenY - s0.y) * u;
        if (mode === 'move') post({ cmd: '$move', x: s0.st.x + dx, y: s0.st.y + dy });
        else post({ cmd: '$resize', w: s0.st.w + dx, h: s0.st.h + dy, ghost: true });
      };
      var move = function (ev) { if (desktop) return; last = ev; if (!raf) raf = requestAnimationFrame(send); };
      var done = function () {
        el.removeEventListener('pointerup', done); el.removeEventListener('pointercancel', done); el.removeEventListener('pointermove', move);
        if (raf) { cancelAnimationFrame(raf); send(); }
        post({ cmd: '$drag', phase: 'end' });
      };
      el.addEventListener('pointerup', done); el.addEventListener('pointercancel', done); el.addEventListener('pointermove', move);
      post({ cmd: '$drag', phase: 'start', mode: mode });
    });
  }
  window.kmod = {
    onmessage: null,
    unit: unit,
    dragMove: function (el) { dragger(el, 'move'); },
    dragResize: function (el) { dragger(el, 'resize'); },
    state: function () { return state; },
    pointer: function (e) { var u = unit(); return { x: e.screenX * u, y: e.screenY * u }; },
    send: function (o) { post(o); },
    close: function () { post({ cmd: '$close' }); },
    fast: function (on) { post({ cmd: '$fast', on: !!on }); },      /* the host reads the page's messages every frame while on (for sending a lot of messages in a row) */
    move: function (x, y) { post({ cmd: '$move', x: x, y: y }); },
    resize: function (w, h) { post({ cmd: '$resize', w: w, h: h }); }
  };
  window.onHost = function (o) {
    if (o && o.cmd === '$state') { state = o; return; }
    if (o && o.cmd === '$scale') {        /* the host knows the UI scale: 1 CSS pixel = 1 UI unit, whatever the window size */
      if (desktop && o.value > 0) { scaled = true; document.documentElement.style.zoom = String(o.value); }
      return;
    }
    if (window.kmod.onmessage) window.kmod.onmessage(o);
  };
  var ready = function () { post({ cmd: '$ready' }); };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ready); else ready();
})();
</script>
]==]

-- inlines the files an HTML page refers to (style sheets, scripts, images, fonts: relative paths only) so the page can be loaded as one data: URL
local function inlineAssets(html, dir)
  local function read(rel)
    if rel:find("..", 1, true) or rel:find("^[/\\]") or rel:find(":", 1, true) then return nil end
    return kmod.readFile(dir .. "/" .. rel)
  end
  local function dataUri(rel)
    local data = read(rel)
    if not data then return nil end
    local ext = rel:match("%.([%w]+)$")
    return "data:" .. (MIME[ext and ext:lower() or ""] or "application/octet-stream") .. ";base64," .. kmod.base64(data)
  end
  local function cssUrls(css)
    return (css:gsub("url%(%s*['\"]?([^'\")]+)['\"]?%s*%)", function(u)
      local d = dataUri(u)
      return d and ("url(" .. d .. ")") or nil
    end))
  end
  -- <link rel="stylesheet" href="x.css">  ->  <style>
  html = html:gsub("<link([^>]-)>", function(attrs)
    if not attrs:find("stylesheet", 1, true) then return nil end
    local href = attrs:match("href%s*=%s*\"([^\"]+)\"") or attrs:match("href%s*=%s*'([^']+)'")
    local css = href and read(href)
    return css and ("<style>" .. cssUrls(css) .. "</style>") or nil
  end)
  -- <script src="x.js"></script>
  html = html:gsub("<script([^>]-)src%s*=%s*\"([^\"]+)\"([^>]-)></script>", function(a, src, b)
    local js = read(src)
    if not js then return nil end
    return "<script" .. a .. b .. ">" .. js:gsub("</script", "<\\/script") .. "</script>"
  end)
  -- src="x.png"
  html = html:gsub("(%ssrc%s*=%s*)\"([^\":]+)\"", function(pre, rel)
    local d = dataUri(rel)
    return d and (pre .. "\"" .. d .. "\"") or nil
  end)
  -- url(x.png) in <style> blocks and style="" attributes
  html = html:gsub("<style(.-)</style>", function(body) return "<style" .. cssUrls(body) .. "</style>" end)
  return html
end

local function buildPage(opts, modDir)
  local html, dir = opts.html, nil
  if not html then
    local base = opts.dir and modDir and (modDir .. "/" .. opts.dir) or nil
    if not base then error("UI.web: give html = \"...\" or dir = \"ui\" (a folder of your mod)", 3) end
    if opts.dir:find("..", 1, true) or opts.dir:find("^[/\\]") then error("UI.web: dir must be a folder inside the mod", 3) end
    dir = base
    html = kmod.readFile(dir .. "/" .. (opts.entry or "index.html"))
    if not html then error("UI.web: cannot read " .. dir .. "/" .. (opts.entry or "index.html"), 3) end
    html = inlineAssets(html, dir)
  end
  local design = tonumber(opts.design_width) or tonumber(opts.width) or 860
  local prelude = WEB_PRELUDE:gsub("@DESIGN@", tostring(math.floor(design)))
  if html:find("<head[^>]*>") then html = html:gsub("(<head[^>]*>)", function(h) return h .. prelude end, 1)
  elseif html:find("<html[^>]*>") then html = html:gsub("(<html[^>]*>)", function(h) return h .. "<head>" .. prelude .. "</head>" end, 1)
  else html = "<!doctype html><html><head><meta charset=\"utf-8\">" .. prelude .. "</head><body>" .. html .. "</body></html>" end
  if #html > MAX_PAGE then error("UI.web: the page is too large (" .. #html .. " bytes, at most " .. MAX_PAGE .. ")", 3) end
  return html
end

local WebWin = {}
WebWin.__index = WebWin

function WebWin:alive() return self.host ~= nil and self.web ~= nil and self.host.uw:IsValid() and self.web:IsValid() end
function WebWin:is_open() return self:alive() and self.open == true end

local function place(self)
  local o = self.opts
  local slot = self.slot
  if o.fullscreen then
    slot:SetAnchors({ Minimum = { X = 0, Y = 0 }, Maximum = { X = 1, Y = 1 } })
    slot:SetOffsets({ Left = 0, Top = 0, Right = 0, Bottom = 0 })
    return
  end
  local ax, ay = 0.5, 0.5
  local p = o.position
  if type(p) == "table" then ax, ay = p.x or p[1] or 0.5, p.y or p[2] or 0.5
  elseif p == "top-left" then ax, ay = 0, 0 elseif p == "top-right" then ax, ay = 1, 0 elseif p == "top" then ax, ay = 0.5, 0
  elseif p == "bottom-left" then ax, ay = 0, 1 elseif p == "bottom-right" then ax, ay = 1, 1 elseif p == "bottom" then ax, ay = 0.5, 1
  elseif p == "left" then ax, ay = 0, 0.5 elseif p == "right" then ax, ay = 1, 0.5 end
  self.ax, self.ay = ax, ay
  slot:SetAnchors({ Minimum = { X = ax, Y = ay }, Maximum = { X = ax, Y = ay } })
  slot:SetAlignment({ X = ax, Y = ay })
  slot:SetAutoSize(true)
  slot:SetPosition({ X = o.x or 0, Y = o.y or 0 })
  if self.gslot then
    self.gslot:SetAnchors({ Minimum = { X = ax, Y = ay }, Maximum = { X = ax, Y = ay } })
    self.gslot:SetAlignment({ X = ax, Y = ay })
    self.gslot:SetAutoSize(true)
  end
end

function WebWin:send(tbl)
  if not self:alive() then return false end
  local js = "window.onHost && window.onHost(" .. kmod.json.encode(tbl) .. ")"
  return pcall(function() self.web:ExecuteJavascript(js) end)
end

local function sendState(self)
  local o = self.opts
  self:send({ cmd = "$state", x = o.x or 0, y = o.y or 0, w = o.width or 0, h = o.height or 0 })
end

-- the window's size limits (opts.min_width / min_height / max_width / max_height)
function WebWin:clamp(w, h)
  local o = self.opts
  local minW, minH = tonumber(o.min_width) or 160, tonumber(o.min_height) or 100
  local maxW, maxH = tonumber(o.max_width) or 8000, tonumber(o.max_height) or 8000
  if maxW < minW then maxW = minW end
  if maxH < minH then maxH = minH end
  return math.max(minW, math.min(maxW, w)), math.max(minH, math.min(maxH, h))
end

-- resizing: the window itself stays as it is, a translucent frame follows the pointer; the window takes the new size once, on release
function WebWin:ghost(w, h)
  w, h = self:clamp(w, h)
  if not (self.gbox and self.gslot) then self:set_size(w, h, true); return end
  local o = self.opts
  self.ghostSize = { w, h }
  pcall(function()
    self.gbox:SetWidthOverride(w); self.gbox:SetHeightOverride(h)
    self.gslot:SetPosition({ X = (o.x or 0) + (self.ax or 0) * (w - (o.width or w)), Y = (o.y or 0) + (self.ay or 0) * (h - (o.height or h)) })
    self.gbox:SetVisibility(4)
    if kmod.platform == "Android" and not self.webHidden then      -- the phone's WebView is a native view above all game widgets: hide it so the frame can be seen
      self.webHidden = true; self.web:SetVisibility(2)
    end
  end)
end

function WebWin:ghost_end(apply)
  local g = self.ghostSize
  self.ghostSize = nil
  if self.gbox then pcall(function() self.gbox:SetVisibility(1) end) end
  if self.webHidden then self.webHidden = false; pcall(function() self.web:SetVisibility(0) end) end
  if g and apply ~= false and self:alive() then self:set_size(g[1], g[2], true) end
end

function WebWin:set_size(w, h, keepOrigin)
  local o = self.opts
  w, h = self:clamp(w, h)
  if keepOrigin and self.ax then            -- the alignment point stays put, so the top-left corner moves by (alignment * growth): move the position back
    self:set_position((o.x or 0) + (self.ax or 0) * (w - (o.width or w)), (o.y or 0) + (self.ay or 0) * (h - (o.height or h)))
  end
  self.opts.width, self.opts.height = w, h
  if self.box then self.box:SetWidthOverride(w); self.box:SetHeightOverride(h) end
  sendState(self)
end

function WebWin:set_position(x, y)
  self.opts.x, self.opts.y = x, y
  if self.slot then pcall(function() self.slot:SetPosition({ X = x, Y = y }) end) end
  sendState(self)
end

function WebWin:show()
  if not self:alive() then return end
  self.host.uw:SetVisibility(0)
  self.open = true
end

function WebWin:hide()
  if not self:alive() then return end
  self.host.uw:SetVisibility(1)
  self.open = false
end

function WebWin:close()
  if self.host and self.host.uw:IsValid() then pcall(function() self.host.uw:RemoveFromParent() end) end
  self.host, self.web, self.open = nil, nil, false
  local list = kmod._web_windows
  for i = #list, 1, -1 do if list[i] == self then table.remove(list, i) end end
end

-- Dragging: while the page says the pointer is down, the host reads the pointer itself every frame and moves / resizes the window.
--   desktop: the mouse position and the state of the left button;  Android: the first finger (position and "still pressed").
local function viewportScale(self)
  if not self.vscale then
    local ok, s = pcall(function()
      return StaticFindObject("/Script/UMG.Default__WidgetLayoutLibrary"):GetViewportScale(self.host.pc)
    end)
    self.vscale = (ok and tonumber(s) and s > 0) and s or 1
  end
  return self.vscale
end

-- x, y in UI units, and whether the pointer is down (nil = cannot tell)
local function pointerState(self)
  local pc = self.host.pc
  if kmod.platform == "Android" then
    local x, y, pressed = pc:GetInputTouchState(0)           -- finger 1 (EETouchIndex 0)
    local s = viewportScale(self)
    return x / s, y / s, pressed and true or false
  end
  local p = StaticFindObject("/Script/UMG.Default__WidgetLayoutLibrary"):GetMousePositionOnViewport(pc)
  return p.X, p.Y, kmod.key_down("LMB")                        -- the OS button state: the game's own input does not see the mouse while the browser widget has it
end

local function beginDrag(self, mode)
  local ok, mx, my, down0 = pcall(pointerState, self)
  if not (ok and mx) then kmod.log("warn", self.owner, "UI.web: cannot read the pointer position: " .. tostring(mx)); return end
  kmod.log("debug", self.owner, string.format("UI.web: %s drag starts at %.1f, %.1f (pressed: %s)", mode, mx, my, tostring(down0)))
  local o = self.opts
  local d = { mode = mode, mx = mx, my = my, x = o.x or 0, y = o.y or 0, w = o.width or 0, h = o.height or 0, t0 = os.clock(), tLast = 0, lw = o.width, lh = o.height }
  self.drag = d
  FastLoop(8, function()
    if self.drag ~= d or not self:alive() or os.clock() - d.t0 > 120 then
      if self.drag == d then self.drag = nil end
      self:ghost_end()
      return true
    end
    local ok2, cx, cy, down = pcall(pointerState, self)
    if ok2 and cx then
      if down == false and os.clock() - d.t0 > 0.15 then self.drag = nil; self:ghost_end(); return true end      -- released (the page may never see it: the pointer left the widget)
      local dx, dy = cx - d.mx, cy - d.my
      if d.mode == "move" then
        self:set_position(d.x + dx, d.y + dy)
      else
        local w, h = self:clamp(math.floor((d.w + dx) / 2 + 0.5) * 2, math.floor((d.h + dy) / 2 + 0.5) * 2)
        if w ~= d.lw or h ~= d.lh then d.lw, d.lh = w, h; self:ghost(w, h) end
      end
    end
    return false
  end)
end

local pollOne
local function beginFastPoll(self, idleStop)
  local t0 = os.clock()
  self.fast, self.fastIdle, self.lastAct = true, idleStop, t0
  FastLoop(8, function()
    if not self.fast or not self:alive() or os.clock() - t0 > 120 or (self.fastIdle and os.clock() - (self.lastAct or t0) > 1.5) then self.fast = false; return true end
    pollOne(self)
    return false
  end)
end

local function handle(self, msg)
  local o = self.opts
  local cmd = type(msg) == "table" and msg.cmd
  if cmd == "$ready" then
    self.ready = true
    pcall(function()                                               -- tell the page how many pixels one UI unit is (the desktop browser lays out in real pixels)
      local lib = StaticFindObject("/Script/UMG.Default__WidgetLayoutLibrary")
      local s = lib:GetViewportScale(self.host.pc)
      if tonumber(s) and s > 0 then self:send({ cmd = "$scale", value = s }) end
    end)
    sendState(self)
    callMod(self.owner, "web ready", o.on_ready)
  elseif cmd == "$close" then
    self:close()
    callMod(self.owner, "web close", o.on_close)
  elseif cmd == "$drag" then
    if msg.phase == "start" then
      if kmod.platform == "Android" then beginFastPoll(self) else beginDrag(self, msg.mode == "resize" and "resize" or "move") end
    else
      self.drag, self.fast = nil, false
      self:ghost_end()
      if self.pendingResize then local r = self.pendingResize; self.pendingResize = nil; self:set_size(r[1], r[2], true) end
    end
  elseif cmd == "$fast" then
    if msg.on then if not self.fast then beginFastPoll(self, true) end else self.fast = false end
  elseif cmd == "$move" then
    if tonumber(msg.x) and tonumber(msg.y) then self:set_position(msg.x, msg.y) end
  elseif cmd == "$resize" then
    local w, h = tonumber(msg.w), tonumber(msg.h)
    if w and h and w > 0 and h > 0 then
      w, h = self:clamp(math.floor(w / 2 + 0.5) * 2, math.floor(h / 2 + 0.5) * 2)
      local now = os.clock()
      if msg.ghost then self:ghost(w, h)
      elseif now - (self.lastResize or 0) >= 0.033 then self.lastResize, self.pendingResize = now, nil; self:set_size(w, h, true)
      else self.pendingResize = { w, h } end      -- resizing makes the browser re-layout: at most ~30 times a second; the newest request is applied by the next poll
    end
  else
    callMod(self.owner, "web message", o.on_message, msg)
  end
end

pollOne = function(self)
  if not self:alive() then
    self:close()
    callMod(self.owner, "web close", self.opts.on_close)        -- the widget is gone (a level change destroys it)
    return
  end
  if self.pendingResize and os.clock() - (self.lastResize or 0) >= 0.033 then
    local r = self.pendingResize
    self.pendingResize, self.lastResize = nil, os.clock()
    self:set_size(r[1], r[2], true)
  end
  local title = textOf(self.web:GetTitleText())
  local seq, body = title:match("^kmod:(%d+):(.*)$")
  if not seq then
    local frag = textOf(self.web:GetUrl()):match("#(kmod[^#]*)$")
    if frag then
      frag = frag:gsub("%%(%x%x)", function(h) return string.char(tonumber(h, 16)) end)
      seq, body = frag:match("^kmod:(%d+):(.*)$")
    end
  end
  seq = tonumber(seq)
  if not seq or seq == self.lastSeq then return end
  self.lastSeq = seq
  self.lastAct = os.clock()
  local ok, msg = pcall(kmod.json.decode, body)
  if ok and msg ~= nil then handle(self, msg)
  else kmod.log("warn", self.owner, "UI.web: a message from the page could not be read (" .. #body .. " bytes" .. (ok and "" or (": " .. tostring(msg))) .. ")") end
end

local function ensureLoop()
  if kmod._web_loop then return end
  kmod._web_loop = true
  LoopAsync(40, function()
    local list = kmod._web_windows
    for i = #list, 1, -1 do
      local w = list[i]
      if w then
        local ok, err = pcall(pollOne, w)
        if not ok then kmod.log("error", w.owner, "UI.web poll failed: " .. tostring(err)) end
      end
    end
    return false
  end)
end

function kmod.ui_web(owner, opts, modDir)
  opts = opts or {}
  local host = newHost()
  local cls = StaticFindObject(WEB_CLASS)
  if not (cls and cls:IsValid()) then error("UI.web: the game has no WebBrowser widget", 2) end
  local page = buildPage(opts, modDir)
  local win = setmetatable({ owner = owner, opts = opts, host = host, open = false, lastSeq = 0 }, WebWin)
  local canvas = spawn(host, "CanvasPanel")
  host.tree.RootWidget = canvas
  local web = host.gp:SpawnObject(cls, host.tree)
  win.web = web
  if opts.transparent ~= false then pcall(function() web.bSupportsTransparency = true end) end     -- before the widget builds itself: pages may draw rounded corners, shadows...
  if opts.fullscreen then
    win.slot = canvas:AddChild(web)
  else
    local box = spawn(host, "SizeBox")
    opts.width, opts.height = win:clamp(opts.width or 860, opts.height or 520)
    box:SetWidthOverride(opts.width)
    box:SetHeightOverride(opts.height)
    box:SetContent(web)
    win.box = box
    win.slot = canvas:AddChild(box)
    local gbox = spawn(host, "SizeBox")                            -- the resize frame
    local gb = spawn(host, "Border")
    gb:SetBrushColor({ R = 0.30, G = 0.55, B = 1.0, A = kmod.platform == "Android" and 0.55 or 0.28 })
    gbox:SetContent(gb)
    win.gslot = canvas:AddChild(gbox)
    gbox:SetVisibility(1)
    win.gbox = gbox
  end
  place(win)
  host.uw:AddToViewport(Z_ORDER + 1)
  table.insert(kmod._web_windows, win)
  ensureLoop()
  -- the widget builds itself a moment after it is added: load the page then
  local loadUrl = "data:text/html;charset=utf-8;base64," .. kmod.base64(page)
  ExecuteWithDelay(350, function()
    if not win:alive() then return end
    local ok, err = pcall(function() web:LoadURL(loadUrl) end)
    if not ok then kmod.log("error", owner, "UI.web: could not load the page: " .. tostring(err)) end
  end)
  if opts.show == false then win:hide() else win.open = true end
  return win
end

-- closes every window (and the toast) of a mod, or of all mods; called when mods reload
function kmod.ui_close_all(owner)
  for id, list in pairs(kmod._ui_windows) do
    if owner == nil or owner == id then
      for i = #list, 1, -1 do pcall(function() list[i]:close() end) end
    end
  end
  local wl = kmod._web_windows
  for i = #wl, 1, -1 do
    if wl[i] and (owner == nil or wl[i].owner == owner) then pcall(function() wl[i]:close() end) end
  end
  if owner == nil and toast and toast.uw and toast.uw:IsValid() then pcall(function() toast.uw:RemoveFromParent() end); toast = nil end
end

-- the per-mod table (see modloader.lua)
function kmod.ui_api(rec)
  return {
    window = function(opts) return kmod.ui_window(rec.id, opts) end,
    toast = function(text, seconds) return kmod.ui_toast(rec.id, text, seconds) end,
    web = function(opts) return kmod.ui_web(rec.id, opts, rec.dir) end,
  }
end

-- ------------------------------------------------------------------ events
local function rec(self) return ctrls[self:GetAddress()] end

function H.click(self)
  local c = rec(self)
  if c and c.kind == "click" and c.win then callMod(c.win.owner, "button", c.cb) end
end
function H.close(self)
  local c = rec(self)
  if c and c.kind == "close" and c.win then callMod(c.win.owner, "close", c.cb) end
end
function H.check(self)
  local c = rec(self)
  if c and c.kind == "check" and not c.win.updating then callMod(c.win.owner, "toggle", c.cb, self:GetIsChecked()) end
end
function H.slide(self)
  local c = rec(self)
  if c and c.kind == "slide" and not c.win.updating then callMod(c.win.owner, "slider", c.cb, self.Slider:GetValue()) end
end
function H.text(self)
  local c = rec(self)
  if c and c.kind == "text" and not c.win.updating then
    local ok, v = pcall(function() return self.EditableTextBox:GetText() end)
    if ok then callMod(c.win.owner, "input", c.cb, tostring(v)) end
  end
end
function H.drop(self)
  local c = rec(self)
  if c and c.kind == "drop" and not c.win.updating then callMod(c.win.owner, "choice", c.cb, c.options[self:GetSelectedIndex() + 1]) end
end

if not kmod._ui_hooked then            -- registered once per Lua state; they dispatch through kmod._uih
  kmod._ui_hooked = true
  RegisterHook(PATH.click, function() end, function(self) kmod._uih.click(self) end)
  RegisterHook(PATH.close, function() end, function(self) kmod._uih.close(self) end)
  RegisterHook(PATH.check, function() end, function(self) kmod._uih.check(self) end)
  RegisterHook(PATH.slide, function() end, function(self) kmod._uih.slide(self) end)
  RegisterHook(PATH.text,  function() end, function(self) kmod._uih.text(self) end)
  RegisterHook(PATH.drop,  function() end, function(self) kmod._uih.drop(self) end)
end
