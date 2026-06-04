function controls()
  return {
    { type = "button", id = "run" },
    { type = "combo", id = "mode", options = { "单次", "循环" } },
    { type = "elf_symbol_combo", id = "target" },
  }
end
