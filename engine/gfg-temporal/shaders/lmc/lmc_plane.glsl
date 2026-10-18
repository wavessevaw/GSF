// Motion-plane helpers shared by lmc_layers (RANSAC) and lmc_irls (the fit).
// Reference: gen_lmc2.fit_h_ls, irls_h.

vec2 blk_centre(int i) {
    int w = lblocks(1).x;
    return vec2(float(i % w) * 8.0 + 3.5, float(i / w) * 8.0 + 3.5);
}

// full-res vector of a level-1 B-grid block
vec2 blk_vec(int i) { return vb[P.blk_off.x + i].xy * 2.0; }

float tol_of(vec2 v) { return 1.5 + 0.03 * length(v); }

// --------------------------------------------------------------- 8x8 solve

void solve8(inout float M[8][8], inout float b[8], out float h[8]) {
    for (int c = 0; c < 8; ++c) {
        int piv = c;
        for (int r = c + 1; r < 8; ++r)
            if (abs(M[r][c]) > abs(M[piv][c])) piv = r;
        if (piv != c) {
            for (int k = 0; k < 8; ++k) { float t = M[c][k]; M[c][k] = M[piv][k]; M[piv][k] = t; }
            float t = b[c]; b[c] = b[piv]; b[piv] = t;
        }
        float d = M[c][c];
        if (abs(d) < 1e-20) d = 1e-20;
        for (int r = c + 1; r < 8; ++r) {
            float f = M[r][c] / d;
            for (int k = c; k < 8; ++k) M[r][k] -= f * M[c][k];
            b[r] -= f * b[c];
        }
    }
    for (int r = 7; r >= 0; --r) {
        float s = b[r];
        for (int k = r + 1; k < 8; ++k) s -= M[r][k] * h[k];
        float d = M[r][r];
        h[r] = s / (abs(d) < 1e-20 ? 1e-20 : d);
    }
}

// normalisation T = [[s, 0, -s cx], [0, s, -s cy], [0, 0, 1]] -> H = Td^-1 Hn Ts
Layer denorm(float h[8], vec3 ns, vec3 nd) {
    mat3 Hn = transpose(mat3(h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], 1.0));
    mat3 Ts = transpose(mat3(ns.z, 0.0, -ns.z * ns.x, 0.0, ns.z, -ns.z * ns.y, 0.0, 0.0, 1.0));
    mat3 Td = transpose(mat3(nd.z, 0.0, -nd.z * nd.x, 0.0, nd.z, -nd.z * nd.y, 0.0, 0.0, 1.0));
    mat3 H = inverse(Td) * Hn * Ts;
    H /= H[2][2];
    mat3 R = transpose(H);
    Layer L;
    L.h0 = vec4(R[0], 0.0);
    L.h1 = vec4(R[1], 0.0);
    L.h2 = vec4(R[2], 0.0);
    return L;
}

// accumulate one correspondence into the normal equations (normalised coords)
void accum(inout float M[8][8], inout float b[8], vec2 x, vec2 u, float w) {
    float r0[8] = float[8](x.x, x.y, 1.0, 0.0, 0.0, 0.0, -u.x * x.x, -u.x * x.y);
    float r1[8] = float[8](0.0, 0.0, 0.0, x.x, x.y, 1.0, -u.y * x.x, -u.y * x.y);
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 8; ++j) M[i][j] += w * (r0[i] * r0[j] + r1[i] * r1[j]);
        b[i] += w * (r0[i] * u.x + r1[i] * u.y);
    }
}
