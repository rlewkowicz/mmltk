struct CaptureUniforms {
  canvas: vec4f,
  frame: vec4f,
  info: vec4f,
  padding: vec4f,
};

@group(0) @binding(0) var<uniform> uniforms: CaptureUniforms;
@group(0) @binding(1) var capture_texture: texture_2d<f32>;
@group(0) @binding(2) var capture_sampler: sampler;

struct VsOut {
  @builtin(position) position: vec4f,
};

@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32) -> VsOut {
  var positions = array<vec2f, 3>(
    vec2f(-1.0, -3.0),
    vec2f(-1.0, 1.0),
    vec2f(3.0, 1.0),
  );

  var output: VsOut;
  output.position = vec4f(positions[vertex_index], 0.0, 1.0);
  return output;
}

fn rect_sdf(point: vec2f, origin: vec2f, size: vec2f) -> f32 {
  let half_size = max(size * 0.5, vec2f(0.5, 0.5));
  let center = origin + half_size;
  let delta = abs(point - center) - half_size;
  return length(max(delta, vec2f(0.0, 0.0))) + min(max(delta.x, delta.y), 0.0);
}

fn fill_alpha(point: vec2f, origin: vec2f, size: vec2f) -> f32 {
  return 1.0 - smoothstep(0.0, 1.25, rect_sdf(point, origin, size));
}

fn border_alpha(point: vec2f, origin: vec2f, size: vec2f, thickness: f32) -> f32 {
  let distance = abs(rect_sdf(point, origin, size));
  return 1.0 - smoothstep(max(0.0, thickness - 1.0), thickness + 1.0, distance);
}

fn workflow_tint(code: f32) -> vec3f {
  if (code < 0.5) {
    return vec3f(0.84, 0.47, 0.24);
  }
  if (code < 1.5) {
    return vec3f(0.34, 0.73, 0.91);
  }
  if (code < 2.5) {
    return vec3f(0.92, 0.74, 0.30);
  }
  if (code < 3.5) {
    return vec3f(0.38, 0.83, 0.72);
  }
  if (code < 4.5) {
    return vec3f(0.88, 0.60, 0.32);
  }
  return vec3f(0.64, 0.76, 0.92);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
  let canvas_size = max(uniforms.canvas.xy, vec2f(1.0, 1.0));
  let frame_origin = uniforms.canvas.zw;
  let frame_size = max(uniforms.frame.xy, vec2f(1.0, 1.0));
  let workflow_code = uniforms.info.x;
  let canvas_point = position.xy;
  let canvas_uv = canvas_point / canvas_size;
  let tint = workflow_tint(workflow_code);

  let background_mix = clamp(canvas_uv.y * 0.82 + canvas_uv.x * 0.18, 0.0, 1.0);
  let vignette = 1.0 - smoothstep(0.32, 0.96, distance(canvas_uv, vec2f(0.5, 0.46)));
  var color = mix(vec3f(0.018, 0.024, 0.029), vec3f(0.052, 0.061, 0.071), background_mix);
  color += tint * (0.032 + vignette * 0.028);

  let frame_alpha = fill_alpha(canvas_point, frame_origin, frame_size);
  let frame_uv = clamp((canvas_point - frame_origin) / frame_size, vec2f(0.0), vec2f(1.0));
  let sampled_surface = textureSample(capture_texture, capture_sampler, frame_uv).rgb;
  color = mix(color, sampled_surface, frame_alpha);

  let frame_border = border_alpha(canvas_point, frame_origin, frame_size, 1.5);
  let frame_inner = border_alpha(canvas_point, frame_origin + vec2f(1.0, 1.0), frame_size - vec2f(2.0, 2.0), 0.75);
  color = mix(color, vec3f(0.78, 0.83, 0.88), frame_border * 0.42);
  color = mix(color, vec3f(0.10, 0.13, 0.16), frame_inner * 0.16);

  return vec4f(color, 1.0);
}
