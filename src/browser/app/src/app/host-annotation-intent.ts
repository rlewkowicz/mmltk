export function hostWorkspaceBoxDragKind(value: unknown): unknown {
  switch (value) {
    case "resize-top-left":
      return "resize_top_left";
    case "resize-top-right":
      return "resize_top_right";
    case "resize-bottom-left":
      return "resize_bottom_left";
    case "resize-bottom-right":
      return "resize_bottom_right";
    default:
      return value;
  }
}
