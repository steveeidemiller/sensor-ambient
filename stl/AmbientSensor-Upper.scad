/* CONFIGURATION */

// Threadded screw hole. Suggested values: M2=1.8, M2.5=2.25, M3=2.7, M4=3.6, M5=4.5
postInnerDiameter = 1.8; // 0.01
// Thickness of the post. Suggested values: M2=3.5, M2.5=4.5, M3=5, M4=7, M5=9
postOuterDiameter = 3.5; // 0.01
// Height of the mounting "post", allowing for airflow, bottom mounted devices, etc.
postHeight = 5;
// Thickness of the base plate
baseHeight = 2.5; // 0.01
// Text
baseCaption = "Ambient-1";
// Text size
baseCaptionSize = 6;
// Level of detail for rendering
$fs = .2; // 0.01


//
baseLength = 100;
baseWidth = 55;




/* PROGRAM */

// Calculated dimensions
postInnerRadius = postInnerDiameter / 2;
postOuterRadius = postOuterDiameter / 2;

// Screw hole
module screwHole(x, y)
{
    screwHoleDiameter = 3;
    translate([x, y, baseHeight / 2])
    {
        cylinder(h = baseHeight + 1, r = screwHoleDiameter / 2 + 0.2, center = true);
    }
}

// Render a "post" as one solid cylinder with a hollow cylinder/hole inside of it
module post()
{
    difference()
    {
        cylinder(postHeight, postOuterRadius, postOuterRadius); // Solid
        cylinder(postHeight+1, postInnerRadius, postInnerRadius); // Hollow it out with a slightly taller cylinder
    }
}

// Render four posts in a rectangular configuration, centered on the platform width
module fourPost(x, length, width)
{
    y = (baseWidth - width) / 2;
    translate([x, y, baseHeight]) { post(); }
    translate([x + length, y, baseHeight]) { post(); }
    translate([x + length, y + width, baseHeight]) { post(); }
    translate([x, y + width, baseHeight]) { post(); }
}

// Device locations
esp32L = 17.5;
esp32W = 45.75;
esp32X = 10.5;
esp32Y = (baseWidth - esp32W) / 2;
micW = 11;
micX = 47; // Post location
micY = (baseWidth - micW) / 2;

// Render the rectangular base with screw holes, space under the ESP32, and embossed text
difference()
{
    // Base
    cube([baseLength, baseWidth, baseHeight]);

    // ESP32 cutout
    translate([esp32X - 3, (baseWidth - esp32W + postOuterDiameter)/2, -1])
    {
        cube([esp32L + 6, esp32W - postOuterDiameter, baseHeight + 2]);
    }

    // SPH0645 cutout
    translate([micX - postOuterDiameter/2 - 10, (baseWidth - 20)/2, -1])
    {
        cube([10, 20, baseHeight + 2]);
    }
 
    // Screw holes around the border used to fasten upper piece to lower piece
    screwHoleOffset = 3;
    screwCount = 4;
    screwInterval = (baseLength - screwHoleOffset * 2) / (screwCount - 1);
    for (i = [0 : 1 : screwCount-1]) // [start: inc: end]
    {
        screwHole(screwHoleOffset + i * screwInterval, screwHoleOffset);
        screwHole(screwHoleOffset + i * screwInterval, baseWidth - screwHoleOffset);
    }

    // Recessed text
    translate([baseLength-7, baseWidth/2, baseHeight-.4])
    {
        rotate([0, 0, 90])
        {
            linear_extrude(height = 0.4+1)
            {
                text(text = baseCaption, font = "Arial", size = baseCaptionSize, valign = "center", halign="center");
            }
        }
    }
}

// ESP32 posts
difference()
{
    union()
    {
        // Both left posts act as one for the difference() function
        translate([esp32X - 1.5/2, esp32Y, baseHeight]) { post(); } // Upper left
        translate([esp32X + 1.5/2 + esp32L, esp32Y, baseHeight]) { post(); } // Lower left
    }
    translate([esp32X + 0.5 - 1.5/2, 0, baseHeight + postHeight - 1])
    {
        // Cutout in left posts for WiFi board
        cube([esp32L - 1 + 1.5, baseWidth / 2, 5]);
    }
}
translate([esp32X + esp32L, esp32Y + esp32W, baseHeight]) { post(); } // Lower right
translate([esp32X, esp32Y + esp32W, baseHeight]) { post(); } // Upper right

// SPH0645 microphone posts
translate([micX, micY, baseHeight])
{
    post();
    translate([0, micW, 0]) { post(); }
}

// BME680 and VEML7700 posts
fourPost(55, 12.5, 20.0);
fourPost(75, 12.5, 20.0);
