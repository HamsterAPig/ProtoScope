function ui()
  return {}
end

function on_open(ctx)
  proto.plot.setup({
    channels = {
      { label = "CH1", unit = "V" }
    }
  })

  proto.plot.push(1, {
    source = "merge",
    samples = {
      { t = 2.0, y = 20.0 }
    }
  })
  proto.plot.push(1, {
    source = "merge",
    samples = {
      { t = 1.0, y = 10.0 }
    }
  })
  proto.plot.push(1, {
    source = "other",
    samples = {
      { t = 3.0, y = 30.0 }
    }
  })
end
