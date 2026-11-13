// Shows the wallpaper unchanged. Useful for checking a host's window setup.
void main() {
    fragColor = vec4(texture(uWallpaper, wallpaperUV(vUV)).rgb, 1.0);
}
