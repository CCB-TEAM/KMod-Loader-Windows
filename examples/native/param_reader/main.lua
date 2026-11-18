-- Native.calls() counts how often the hooked function ran; Native.last() is the last text it saw.
ExecuteWithDelay(8000, function()
  print("hooked calls so far: " .. Native.calls() .. ", last A = [" .. Native.last() .. "]")
end)
