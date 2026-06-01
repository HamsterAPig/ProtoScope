function ui()
  return {
    {
      id = "duplicate_label_form",
      title = "重复标签表单",
      controls = {
        { type = "input_text", id = "src_addr", label = "地址", default = "01" },
        { type = "input_text", id = "dst_addr", label = "地址", default = "02" },
      },
      layout = {
        kind = "form",
        items = {
          { controls = { "src_addr", "dst_addr" } },
        }
      }
    },
    {
      id = "duplicate_label_flow",
      title = "重复标签默认流",
      controls = {
        { type = "button", id = "read_src", label = "读取" },
        { type = "button", id = "read_dst", label = "读取" },
      }
    }
  }
end
