# 3D Printer Assets
The `.scad` files are parametric designs for a simple mounting bracket consisting of upper and lower sections. They're built using [OpenSCAD](https://openscad.org/). The open architecture of the mount maximizes exposure for each sensor module.

The ESP32 and each of the sensor modules mount to the round posts on the upper part using M2x4 or M2x5 screws. While four posts are provided for each board, two screws placed diagonally on each should be sufficient. The upper part fastens to the lower part using M3x8 or M3x10 screws. The LiPo battery should be safely contained between the two parts. If the battery will be subject to vibration or movement, it can be secured to the back of the upper part using double-sided tape such as 3M VHB tape.

The lower part can be mounted using various methods. It has conical holes for standard flat-head wood screws. It is recommended that the screw heads are sufficiently recessed to avoid damage to the battery. There is also a single hole along the top of the part that can be used to hang it off of a small nail, 3M Command Hook, etc. Lastly, double-sided tape could also be used.

STL files are provided for each of the two parts. While the design file for the upper part allows for customizable embossed text, the STL file provided here has no text.
