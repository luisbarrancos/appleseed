
//
// This source file is part of appleseed.
// Visit http://appleseedhq.net/ for additional information and resources.
//
// This software is released under the MIT license.
//
// Copyright (c) 2016 Luis Barrancos, The appleseedhq Organization
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
 
#ifndef AS_TEXTURE_HELPERS_H
#define AS_TEXTURE_HELPERS_H

#include "appleseed/math/as_math_helpers.h"

#define NUM_UDIM_NAMES  10
#define NUM_UDIM_ROWS   10

#define DEBUG_CONSTANT_FOLDING(NAMES,J,K)                           \
    string shadername = "";                                         \
    getattribute("shader:shadername", shadername);                  \
                                                                    \
    if (!isconstant(NAMES[10 * J + K]))                             \
    {                                                               \
        warning("[WARNING] Constant NOT folded in %s, %s:%i\n",     \
                shadername, __FILE__, __LINE__);                    \
    }

void udim_mari_filenames(
    string filename,
    string extension,
    output string names[NUM_UDIM_NAMES * NUM_UDIM_ROWS])
{
    for (int i = 0; i < NUM_UDIM_ROWS; ++i)
    {
        for (int j = 0; j < NUM_UDIM_NAMES; ++j)
        {
            names[10 * i + j] =
                format("%s%d.%s", filename, (10 * i + j) + 1001, extension);
#ifdef DEBUG
            DEBUG_CONSTANT_FOLDING(names, i, j)
#endif
        }
    }
}

void udim_filenames(
    string filename,
    string extension,
    output string names[NUM_UDIM_NAMES * NUM_UDIM_ROWS])
{
    for (int i = 0; i < NUM_UDIM_ROWS; ++i)
    {
        for (int j = 0; j < NUM_UDIM_NAMES; ++j)
        {
            names[10 * i + j] =
                format("%su%iv%i.%s", filename, j, i, extension);
#ifdef DEBUG
            DEBUG_CONSTANT_FOLDING(names, i, j)
#endif
        }
    }
}

color textureatlas(
    string filename,
    string style,
    float s,
    float t,
    float blur,
    float width,
    int firstchannel,
    float fill,
    color missingcolor,
    float missingalpha,
    string filter,
    output float alpha
    )
{
    string filenames[NUM_UDIM_NAMES * NUM_UDIM_ROWS] = {""};
    string lookup = "";

    int u_tile = int(s);
    int v_tile = int(t);
    int ndx;

    if (style == "zbrush" || style == "mudbox")
    {
        if (style == "zbrush")
        {
            ndx = 10 * v_tile + u_tile;
        }
        else if (style == "mudbox")
        {
            ndx = 10 * (v_tile + 1) + (u_map + 1);
        }
        udim_filenames(filename, extension, filenames);
        lookup = filenames[ndx];
    }
    else if (style == "mudbox")
    {
        ndx = 10 * v_tile + u_tile;
        udim_mari_filenames(filename, extension, filenames);
        lookup = filenames[ndx];
    }
    else if (style == "explicit")
    {
        ; //
    }
    else
    {
#ifdef DEBUG
        string shadername = "";
        getattribute("shader:shadername", shadername);
        warning("[WARNING]:no valid UDIM style set in %s, %s:%i\n",
                shadername, __FILE__, __LINE__);
#endif
    }

    if (lookup != "")
    {
        return (color) texture(
            lookup,
            s - u_tile,
            1 - (t - v_tile),
            "blur", blur,
            "width", width,
            "firstchannel", firstchannel,
            "fill", fill,
            "missingcolor", missingcolor,
            "missingalpha", missingalpha,
            "alpha", alpha,
            "interp", filter
            );
    }
    else
    {
        return 0;
    }
}

float textureatlas(
    string filename,
    string style,
    float s,
    float t,
    float blur,
    float width,
    int firstchannel,
    string filter
    )
{
    string filenames[NUM_UDIM_NAMES * NUM_UDIM_ROWS] = {""};
    string lookup = "";

    int u_tile = int(s);
    int v_tile = int(t);
    int ndx;

    if (style == "zbrush" || style == "mudbox")
    {
        if (style == "zbrush")
        {
            ndx = 10 * v_tile + u_tile;
        }
        else if (style == "mudbox")
        {
            ndx = 10 * (v_tile + 1) + (u_map + 1);
        }
        udim_filenames(filename, extension, filenames);
        lookup = filenames[ndx];
    }
    else if (style == "mudbox")
    {
        ndx = 10 * v_tile + u_tile;
        udim_mari_filenames(filename, extension, filenames);
        lookup = filenames[ndx];
    }
    else if (style == "explicit")
    {
        ; //
    }
    else
    {
#ifdef DEBUG
        string shadername = "";
        getattribute("shader:shadername", shadername);
        warning("[WARNING]:no valid UDIM style set in %s, %s:%i\n",
                shadername, __FILE__, __LINE__);
#endif
    }

    if (lookup != "")
    {
        return (float) texture(
            lookup,
            s - u_tile,
            1 - (t - v_tile),
            "blur", blur,
            "width", width,
            "firstchannel", firstchannel,
            "fill", fill,
            "interp", filter
            );
    }
    else
    {
        return 0;
    }
}

#endif // AS_TEXTURE_HELPERS_H

