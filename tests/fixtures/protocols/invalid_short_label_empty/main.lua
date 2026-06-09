function ui()
  return {
    {
      id = "invalid_short_label_empty",
      title = "非法 short_label 空值",
      controls = {
        { type = "button", id = "read_version", label = "读取版本", short_label = "" },
      },
    },
  }
end
