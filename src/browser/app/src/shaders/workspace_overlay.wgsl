struct OverlayUniforms {
  canvas: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: OverlayUniforms;

struct VertexInput {
  @location(0) quad: vec2f,
  @location(1) meta0: vec4f,
  @location(2) meta1: vec4f,
  @location(3) meta2: vec4f,
  @location(4) meta3: vec4f,
  @location(5) meta4: vec4f,
  @location(6) meta5: vec4f,
};

struct VsOut {
  @builtin(position) position: vec4f,
  @location(0) canvas_point: vec2f,
  @location(1) kind: f32,
  @location(2) start: vec2f,
  @location(3) end: vec2f,
  @location(4) center: vec2f,
  @location(5) radius: f32,
  @location(6) line_width: f32,
  @location(7) dash: vec2f,
  @location(8) fill_color: vec4f,
  @location(9) stroke_color: vec4f,
};

@vertex
fn vs_main(input: VertexInput) -> VsOut {
  let bounds_origin = vec2f(input.meta0.y, input.meta0.z);
  let bounds_size = max(vec2f(input.meta0.w, input.meta1.x), vec2f(0.5, 0.5));
  let canvas_point = bounds_origin + input.quad * bounds_size;
  let canvas_size = max(uniforms.canvas.xy, vec2f(1.0, 1.0));
  let clip_position = vec2f(
    (canvas_point.x / canvas_size.x) * 2.0 - 1.0,
    1.0 - (canvas_point.y / canvas_size.y) * 2.0,
  );

  var output: VsOut;
  output.position = vec4f(clip_position, 0.0, 1.0);
  output.canvas_point = canvas_point;
  output.kind = input.meta0.x;
  output.start = vec2f(input.meta1.y, input.meta1.z);
  output.end = vec2f(input.meta1.w, input.meta2.x);
  output.center = vec2f(input.meta2.y, input.meta2.z);
  output.radius = input.meta2.w;
  output.line_width = input.meta3.x;
  output.dash = vec2f(input.meta3.y, input.meta3.z);
  output.fill_color = vec4f(
    input.meta3.w,
    input.meta4.x,
    input.meta4.y,
    input.meta4.z,
  );
  output.stroke_color = vec4f(
    input.meta4.w,
    input.meta5.x,
    input.meta5.y,
    input.meta5.z,
  );
  return output;
}

fn saturate(value: f32) -> f32 {
  return clamp(value, 0.0, 1.0);
}

fn composite(base: vec4f, layer: vec4f) -> vec4f {
  let alpha = layer.a + base.a * (1.0 - layer.a);
  if (alpha <= 0.0001) {
    return vec4f(0.0);
  }
  let rgb =
    (layer.rgb * layer.a + base.rgb * base.a * (1.0 - layer.a)) / alpha;
  return vec4f(rgb, alpha);
}

fn distance_to_segment(point: vec2f, start: vec2f, end: vec2f) -> f32 {
  let delta = end - start;
  let length_squared = max(dot(delta, delta), 0.0001);
  let t = saturate(dot(point - start, delta) / length_squared);
  let projection = start + delta * t;
  return distance(point, projection);
}

fn line_coverage(point: vec2f, start: vec2f, end: vec2f, line_width: f32) -> f32 {
  let half_width = max(line_width * 0.5, 0.5);
  let distance_to_line = distance_to_segment(point, start, end);
  return 1.0 - smoothstep(half_width, half_width + 1.0, distance_to_line);
}

fn dash_coverage(point: vec2f, start: vec2f, end: vec2f, dash: vec2f) -> f32 {
  if (dash.x <= 0.0 || dash.y <= 0.0) {
    return 1.0;
  }
  let delta = end - start;
  let segment_length = length(delta);
  if (segment_length <= 0.0001) {
    return 1.0;
  }
  let direction = delta / segment_length;
  let cycle = max(dash.x + dash.y, 0.0001);
  let along = dot(point - start, direction);
  let position = along - floor(along / cycle) * cycle;
  return select(0.0, 1.0, position <= dash.x);
}

fn circle_fill_coverage(point: vec2f, center: vec2f, radius: f32) -> f32 {
  return 1.0 - smoothstep(radius - 1.0, radius + 1.0, distance(point, center));
}

fn circle_stroke_coverage(
  point: vec2f,
  center: vec2f,
  radius: f32,
  line_width: f32,
) -> f32 {
  let half_width = max(line_width * 0.5, 0.5);
  let edge_distance = abs(distance(point, center) - radius);
  return 1.0 - smoothstep(half_width, half_width + 1.0, edge_distance);
}

@fragment
fn fs_main(input: VsOut) -> @location(0) vec4f {
  if (input.kind < 0.5) {
    return input.fill_color;
  }

  if (input.kind < 1.5) {
    let coverage =
      line_coverage(input.canvas_point, input.start, input.end, input.line_width) *
      dash_coverage(input.canvas_point, input.start, input.end, input.dash);
    return vec4f(input.stroke_color.rgb, input.stroke_color.a * coverage);
  }

  var color = vec4f(0.0);
  if (input.fill_color.a > 0.0) {
    color = composite(
      color,
      vec4f(
        input.fill_color.rgb,
        input.fill_color.a *
          circle_fill_coverage(input.canvas_point, input.center, input.radius),
      ),
    );
  }
  if (input.stroke_color.a > 0.0 && input.line_width > 0.0) {
    color = composite(
      color,
      vec4f(
        input.stroke_color.rgb,
        input.stroke_color.a *
          circle_stroke_coverage(
            input.canvas_point,
            input.center,
            input.radius,
            input.line_width,
          ),
      ),
    );
  }
  return color;
}

