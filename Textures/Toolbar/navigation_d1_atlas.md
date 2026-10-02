# D1 navigation artwork

`navigation_d1_atlas.png` is the original approved D1 concept, preserved intact
so the navigation retains the approved bronze corners and background texture.
It was generated with the built-in imagegen tool during the design review.
The second, simplified D2 concept is not used.

`SourceFiles/ui/app_navigation.h` samples pixel rectangles from this 1536 x 1024
atlas. Background texture is tiled and corners retain their proportions.
The book, compass and armor now use hand-authored ImGui paths on a shared
24-unit grid, following the approved flat icon proposal. Their color changes
with interaction state; the original raster emblems are no longer sampled.
Text, interaction feedback, the frame and the selection diamond are rendered by
ImGui, so hit targets and labels remain independent of the bitmap.

Do not resize or replace this image without updating the atlas coordinates.
The existing release packager includes the entire Textures directory.
