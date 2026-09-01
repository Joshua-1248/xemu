/*
 * Texture-pack material sidecar preview.
 *
 * Copy/rename to <hash>.shader beside:
 *   <hash>.png, <hash>_n.png, <hash>_s.png, <hash>_d.png, <hash>_ao.png
 *
 * This deliberately previews the maps in texture space. It is NOT the future
 * draw-time material renderer and therefore does not use the Xbox game's real
 * lights, view vector, tangent basis, or geometry displacement.
 */
void main()
{
    vec2 p = uv;

    if (iHasDisplacementMap) {
        float height = texture(iDisplacementMap, p).r - 0.5;
        p += vec2(0.012, -0.009) * height;
    }

    vec4 base = iHasChannel0 ? texture(iChannel0, p) : vec4(1.0);
    float ao = iHasAOMap ? texture(iAOMap, p).r : 1.0;

    vec3 n = vec3(0.0, 0.0, 1.0);
    if (iHasNormalMap) {
        n = normalize(texture(iNormalMap, p).xyz * 2.0 - 1.0);
    }

    float specMask = 0.0;
    if (iHasSpecularMap) {
        vec3 s = texture(iSpecularMap, p).rgb;
        specMask = dot(s, vec3(0.3333333));
    }

    vec3 lightDir = normalize(vec3(-0.40, -0.35, 0.85));
    vec3 halfDir = normalize(lightDir + vec3(0.0, 0.0, 1.0));
    float diffuse = iHasNormalMap ? (0.60 + 0.40 * max(dot(n, lightDir), 0.0)) : 1.0;
    float specular = iHasNormalMap
        ? pow(max(dot(n, halfDir), 0.0), 32.0) * specMask
        : 0.15 * specMask;

    fragColor = vec4(base.rgb * ao * diffuse + vec3(specular * 0.35), base.a);
}
