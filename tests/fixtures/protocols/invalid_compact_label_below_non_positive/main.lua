function ui()
  return {
    {
      id = "invalid_compact_label_below_non_positive",
      title = "非法 compact_label_below 非正值",
      controls = {
        { type = "button", id = "read_version", label = "读取版本", short_label = "读", compact_label_below = 0 },
      },
    },
  }
end
