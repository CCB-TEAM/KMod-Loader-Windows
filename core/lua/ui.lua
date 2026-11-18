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

-- closes every window (and the toast) of a mod, or of all mods; called when mods reload
function kmod.ui_close_all(owner)
  for id, list in pairs(kmod._ui_windows) do
    if owner == nil or owner == id then
      for i = #list, 1, -1 do pcall(function() list[i]:close() end) end
    end
  end
  if owner == nil and toast and toast.uw and toast.uw:IsValid() then pcall(function() toast.uw:RemoveFromParent() end); toast = nil end
end

-- the per-mod table (see modloader.lua)
function kmod.ui_api(rec)
  return {
    window = function(opts) return kmod.ui_window(rec.id, opts) end,
    toast = function(text, seconds) return kmod.ui_toast(rec.id, text, seconds) end,
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
