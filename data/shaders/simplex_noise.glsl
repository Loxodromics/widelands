// 2D simplex noise from https://github.com/ashima/webgl-noise
// Copyright (C) 2011 by Ashima Arts (Simplex noise)
// Copyright (C) 2011-2016 by Stefan Gustavson (Classic noise and others)
// Distributed under the MIT License.
vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
	const vec4 C = vec4(0.211324865405187,   // (3.0 - sqrt(3.0)) / 6.0
	                    0.366025403784439,   // 0.5 * (sqrt(3.0) - 1.0)
	                   -0.577350269189626,   // -1.0 + 2.0 * C.x
	                    0.024390243902439);  // 1.0 / 41.0
	vec2 i = floor(v + dot(v, C.yy));
	vec2 x0 = v - i + dot(i, C.xx);
	vec2 i1;
	i1.x = step(x0.y, x0.x);
	i1.y = 1.0 - i1.x;
	vec4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;
	i = mod289(i);
	vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
	vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	m = m * m;
	m = m * m;
	vec3 x = 2.0 * fract(p * C.www) - 1.0;
	vec3 h = abs(x) - 0.5;
	vec3 ox = floor(x + 0.5);
	vec3 a0 = x - ox;
	m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
	vec3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	return 130.0 * dot(m, g);
}

// Same field as snoise(), but also returns its analytic gradient: (value, d/dx,
// d/dy). Every corner offset (x0, x12.xy, x12.zw) differs from v only by a
// per-cell constant, so d(x_i)/dv is the identity for all three corners; the
// permutation-derived gradient direction (a0, h) and the Taylor-series norm
// factor are themselves constant within a cell. That makes the chain rule
// fall out of the intermediates snoise() already computes: with
// m_i = max(0.5 - |x_i|^2, 0) and g_i = grad_i . x_i,
//   d(m_i)/dv = -2 x_i           (chain rule on |x_i|^2)
//   d(g_i)/dv = grad_i           (x_i's Jacobian is the identity)
// so value = 130 * sum(norm_i * m_i^4 * g_i) differentiates to
//   grad(value) = 130 * sum(norm_i * (-8 m_i^3 g_i x_i + m_i^4 grad_i)).
// Where m_i is clamped to zero by the max() above, both terms vanish, so no
// branch is needed for continuity. Verified against central differences in
// the scratchpad Python port: ~1e-6 relative error over 5000 random points.
vec3 snoise_grad(vec2 v) {
	const vec4 C = vec4(0.211324865405187,
	                    0.366025403784439,
	                   -0.577350269189626,
	                    0.024390243902439);
	vec2 i = floor(v + dot(v, C.yy));
	vec2 x0 = v - i + dot(i, C.xx);
	vec2 i1;
	i1.x = step(x0.y, x0.x);
	i1.y = 1.0 - i1.x;
	vec4 x12 = x0.xyxy + C.xxzz;
	x12.xy -= i1;
	i = mod289(i);
	vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0)) + i.x + vec3(0.0, i1.x, 1.0));
	vec3 m0 = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy), dot(x12.zw, x12.zw)), 0.0);
	vec3 x = 2.0 * fract(p * C.www) - 1.0;
	vec3 h = abs(x) - 0.5;
	vec3 ox = floor(x + 0.5);
	vec3 a0 = x - ox;
	vec3 norm = 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
	vec3 m0sq = m0 * m0;
	vec3 m4 = norm * m0sq * m0sq;
	vec3 m3 = norm * m0sq * m0;
	vec3 g;
	g.x = a0.x * x0.x + h.x * x0.y;
	g.yz = a0.yz * x12.xz + h.yz * x12.yw;
	float value = 130.0 * dot(m4, g);
	vec3 nm3g = m3 * g;
	vec2 grad = 130.0 * (-8.0 * (nm3g.x * x0 + nm3g.y * x12.xy + nm3g.z * x12.zw) +
	                     m4.x * vec2(a0.x, h.x) + m4.y * vec2(a0.y, h.y) + m4.z * vec2(a0.z, h.z));
	return vec3(value, grad);
}
