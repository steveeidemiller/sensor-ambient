/* CONFIGURATION */

// Threadded screw hole. Suggested values: M2=1.8, M2.5=2.25, M3=2.7, M4=3.6, M5=4.5
postInnerDiameter = 2.7; // 0.01
// Height of the mounting "post", allowing for airflow, bottom mounted devices, etc.
postHeight = 12;
// Thickness of the base plate
baseHeight = 2.5; // 0.01
// Level of detail for rendering
$fs = .2; // 0.01


//
baseLength = 100;
baseWidth = 55;

// Render a "post" as one solid cube with a hollow cylinder/hole inside of it
module post()
{
    difference()
    {
        cube([postOuterWidth, postOuterWidth, postHeight]);
        translate([screwHoleOffset, screwHoleOffset, postHeight / 2])
        {
            // Hollow it out with a slightly taller cylinder
            cylinder(h = postHeight + 1, r = postInnerDiameter / 2, center = true);
        }
    }
}

// Base with mounting holes
difference()
{
    // Base
    cube([baseLength, baseWidth, baseHeight]);

    // Screw holes
    translate([baseLength / 3, baseWidth / 2, -1])
    {
        cylinder(baseHeight+2, r1=3/2, r2=12/2, true);
    }
    translate([baseLength * 2/3, baseWidth / 2, -1])
    {
        cylinder(baseHeight+2, r1=3/2, r2=12/2, true);
    }

    // Hanger hole
    translate([6, baseWidth / 2, -1])
    {
        cylinder(baseHeight + 2, r=2);
    }
}

// Render all posts
screwHoleOffset = 3;
screwCount = 4;
postOuterWidth = screwHoleOffset * 2;
screwInterval = (baseLength - screwHoleOffset * 2) / (screwCount - 1);
for (i = [0 : 1 : screwCount-1]) // [start: inc: end]
{
    translate([i * screwInterval, 0, baseHeight])
    {
        post();
    }
    translate([i * screwInterval, baseWidth - postOuterWidth, baseHeight])
    {
        post();
    }
}

// Battery retainer. NOTE: baseHeight is being used for the thickness of the retainer.
translate([baseLength - baseHeight, baseWidth / 4, baseHeight])
{
    cube([baseHeight, baseWidth / 2, postHeight]);
}
