function ui()
  return {
    {
      id = "invalid_compact_label_below_type",
      title = "非法 compact_label_below 类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本", short_label = "读", compact_label_below = "narrow" },
      },
    },
  }
end
