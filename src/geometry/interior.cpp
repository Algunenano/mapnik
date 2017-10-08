/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2017 Artem Pavlenko
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include <mapnik/geometry/interior.hpp>
#include <mapnik/geometry_envelope.hpp>
#include <mapnik/box2d.hpp>
#include <mapnik/geometry_centroid.hpp>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <queue>

namespace mapnik { namespace geometry {

// Interior algorithm is realized as a modification of Polylabel algorithm
// from https://github.com/mapbox/polylabel.
// The modification aims to improve visual output by prefering
// placements closer to centroid.

namespace detail {

// get squared distance from a point to a segment
template <class T>
T segment_dist_sq(const point<T>& p,
                  const point<T>& a,
                  const point<T>& b)
{
    auto x = a.x;
    auto y = a.y;
    auto dx = b.x - x;
    auto dy = b.y - y;

    if (dx != 0 || dy != 0) {

        auto t = ((p.x - x) * dx + (p.y - y) * dy) / (dx * dx + dy * dy);

        if (t > 1) {
            x = b.x;
            y = b.y;

        } else if (t > 0) {
            x += dx * t;
            y += dy * t;
        }
    }

    dx = p.x - x;
    dy = p.y - y;

    return dx * dx + dy * dy;
}

template <class T>
void point_to_ring_dist(point<T> const& point, linear_ring<T> const& ring,
                        bool & inside, double & min_dist_sq)
{
    for (std::size_t i = 0, len = ring.size(), j = len - 1; i < len; j = i++)
    {
        const auto& a = ring[i];
        const auto& b = ring[j];

        if ((a.y > point.y) != (b.y > point.y) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x)) inside = !inside;

        min_dist_sq = std::min(min_dist_sq, segment_dist_sq(point, a, b));
    }
}

// signed distance from point to polygon outline (negative if point is outside)
template <class T>
double point_to_polygon_dist(const point<T>& point, const polygon<T>& polygon)
{
    bool inside = false;
    double min_dist_sq = std::numeric_limits<double>::infinity();

    point_to_ring_dist(point, polygon.exterior_ring, inside, min_dist_sq);

    for (const auto& ring : polygon.interior_rings)
    {
        point_to_ring_dist(point, ring, inside, min_dist_sq);
    }

    return (inside ? 1 : -1) * std::sqrt(min_dist_sq);
}

template <class T>
struct fitness_functor
{
    fitness_functor(point<T> const& centroid, point<T> const& polygon_size)
        : centroid(centroid),
          max_size(std::max(polygon_size.x, polygon_size.y))
        {}

    T operator()(point<T> const& cell_center, T distance_polygon) const
    {
        if (distance_polygon <= 0)
        {
            return distance_polygon;
        }
        point<T> d(cell_center.x - centroid.x, cell_center.y - centroid.y);
        double distance_centroid = std::sqrt(d.x * d.x + d.y * d.y);
        return distance_polygon * (1 - distance_centroid / max_size);
    }

    point<T> centroid;
    T max_size;
};

template <class T>
struct cell
{
    template <class FitnessFunc>
    cell(const point<T>& c_, T h_,
         const polygon<T>& polygon,
         const FitnessFunc& ff)
        : c(c_),
          h(h_),
          d(point_to_polygon_dist(c, polygon)),
          fitness(ff(c, d)),
          max_fitness(ff(c, d + h * std::sqrt(2)))
        {}

    point<T> c; // cell center
    T h; // half the cell size
    T d; // distance from cell center to polygon
    T fitness; // fitness of the cell center
    T max_fitness; // a "potential" of the cell calculated from max distance to polygon within the cell
};

template <class T>
point<T> polylabel(const polygon<T>& polygon, T precision = 1)
{
    // find the bounding box of the outer ring
    const box2d<T> bbox = envelope(polygon.exterior_ring);
    const point<T> size { bbox.width(), bbox.height() };

    const T cell_size = std::min(size.x, size.y);
    T h = cell_size / 2;

    // a priority queue of cells in order of their "potential" (max distance to polygon)
    auto compare_func = [] (const cell<T>& a, const cell<T>& b)
    {
        return a.max_fitness < b.max_fitness;
    };
    using Queue = std::priority_queue<cell<T>, std::vector<cell<T>>, decltype(compare_func)>;
    Queue queue(compare_func);

    if (cell_size == 0)
    {
        return { bbox.minx(), bbox.miny() };
    }

    point<T> centroid;
    if (!mapnik::geometry::centroid(polygon, centroid))
    {
        auto center = bbox.center();
        return { center.x, center.y };
    }

    fitness_functor<T> fitness_func(centroid, size);

    // cover polygon with initial cells
    for (T x = bbox.minx(); x < bbox.maxx(); x += cell_size)
    {
        for (T y = bbox.miny(); y < bbox.maxy(); y += cell_size)
        {
            queue.push(cell<T>({x + h, y + h}, h, polygon, fitness_func));
        }
    }

    // take centroid as the first best guess
    auto best_cell = cell<T>(centroid, 0, polygon, fitness_func);

    while (!queue.empty())
    {
        // pick the most promising cell from the queue
        auto current_cell = queue.top();
        queue.pop();

        // update the best cell if we found a better one
        if (current_cell.fitness > best_cell.fitness)
        {
            best_cell = current_cell;
        }

        // do not drill down further if there's no chance of a better solution
        if (current_cell.max_fitness - best_cell.fitness <= precision) continue;

        // split the cell into four cells
        h = current_cell.h / 2;
        queue.push(cell<T>({current_cell.c.x - h, current_cell.c.y - h}, h, polygon, fitness_func));
        queue.push(cell<T>({current_cell.c.x + h, current_cell.c.y - h}, h, polygon, fitness_func));
        queue.push(cell<T>({current_cell.c.x - h, current_cell.c.y + h}, h, polygon, fitness_func));
        queue.push(cell<T>({current_cell.c.x + h, current_cell.c.y + h}, h, polygon, fitness_func));
    }

    return best_cell.c;
}

} // namespace detail

template <class T>
point<T> interior(polygon<T> const& polygon, double scale_factor)
{
    // This precision has been chosen to work well in the map (viewport) coordinates.
    double precision = 10.0 * scale_factor;
    return detail::polylabel(polygon, precision);
}

template
point<double> interior(polygon<double> const& polygon, double scale_factor);

} }

