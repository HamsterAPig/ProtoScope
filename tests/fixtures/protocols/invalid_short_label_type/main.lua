function ui()
  return {
    {
      id = "invalid_short_label_type",
      title = "非法 short_label 类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本", short_label = 1 },
      },
    },
  }
end
