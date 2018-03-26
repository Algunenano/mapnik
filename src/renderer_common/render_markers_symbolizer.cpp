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

#include <mapnik/label_collision_detector.hpp>
#include <mapnik/svg/svg_storage.hpp>
#include <mapnik/svg/svg_path_adapter.hpp>
#include <mapnik/marker_cache.hpp>
#include <mapnik/marker_helpers.hpp>
#include <mapnik/renderer_common/render_markers_symbolizer.hpp>
#include <mapnik/symbolizer.hpp>

namespace mapnik {

namespace detail {

template <typename Detector, typename RendererType, typename ContextType>
struct render_marker_symbolizer_visitor
{
    using vector_dispatch_type = vector_markers_dispatch<Detector>;
    using raster_dispatch_type = raster_markers_dispatch<Detector>;

    render_marker_symbolizer_visitor(std::string const& filename,
                                     markers_symbolizer const& sym,
                                     mapnik::feature_impl & feature,
                                     proj_transform const& prj_trans,
                                     RendererType const& common,
                                     box2d<double> const& clip_box,
                                     ContextType & renderer_context)
        : filename_(filename),
          sym_(sym),
          feature_(feature),
          prj_trans_(prj_trans),
          common_(common),
          clip_box_(clip_box),
          renderer_context_(renderer_context) {}

    svg_attribute_type const& get_marker_attributes(svg_path_ptr const& stock_marker,
                                                    svg_attribute_type & custom_attr) const
    {
        auto const& stock_attr = stock_marker->attributes();
        if (push_explicit_style(stock_attr, custom_attr, sym_, feature_, common_.vars_))
            return custom_attr;
        else
            return stock_attr;
    }

    template <typename Marker, typename Dispatch>
    void render_marker(Marker const& mark, Dispatch & rasterizer_dispatch) const
    {
        auto const& vars = common_.vars_;

        agg::trans_affine geom_tr;
        if (auto geometry_transform = get_optional<transform_type>(sym_, keys::geometry_transform))
        {
            evaluate_transform(geom_tr, feature_, vars, *geometry_transform, common_.scale_factor_);
        }

        vertex_converter_type converter(clip_box_,
                                        sym_,
                                        common_.t_,
                                        prj_trans_,
                                        geom_tr,
                                        feature_,
                                        vars,
                                        common_.scale_factor_);

        bool clip = get<value_bool, keys::clip>(sym_, feature_, vars);
        double offset = get<value_double, keys::offset>(sym_, feature_, vars);
        double simplify_tolerance = get<value_double, keys::simplify_tolerance>(sym_, feature_, vars);
        double smooth = get<value_double, keys::smooth>(sym_, feature_, vars);

        if (clip)
        {
            geometry::geometry_types type = geometry::geometry_type(feature_.get_geometry());
            switch (type)
            {
                case geometry::geometry_types::Polygon:
                case geometry::geometry_types::MultiPolygon:
                    converter.template set<clip_poly_tag>();
                    break;
                case geometry::geometry_types::LineString:
                case geometry::geometry_types::MultiLineString:
                    converter.template set<clip_line_tag>();
                    break;
                default:
                    // silence warning: 4 enumeration values not handled in switch
                    break;
            }
        }

        converter.template set<transform_tag>(); //always transform
        if (std::fabs(offset) > 0.0) converter.template set<offset_transform_tag>(); // parallel offset
        converter.template set<affine_transform_tag>(); // optional affine transform
        if (simplify_tolerance > 0.0) converter.template set<simplify_tag>(); // optional simplify converter
        if (smooth > 0.0) converter.template set<smooth_tag>(); // optional smooth converter

        apply_markers_multi(feature_, vars, converter, rasterizer_dispatch, sym_);
    }

    void operator() (marker_null const&) const {}

    void operator() (marker_svg const& mark) const
    {
        using namespace mapnik::svg;
        METRIC_UNUSED auto t = renderer_context_.metrics_.measure_time("Agg_RMS_visit_svg"); /* TODO: Remove this */

        // https://github.com/mapnik/mapnik/issues/1316
        bool snap_to_pixels = !mapnik::marker_cache::instance().is_uri(filename_);

        agg::trans_affine image_tr = agg::trans_affine_scaling(common_.scale_factor_);

        boost::optional<svg_path_ptr> const& stock_vector_marker = mark.get_data();
        svg_path_ptr marker_ptr = *stock_vector_marker;

        svg_attribute_ptr r_attributes = nullptr;

        // Look up the feature/symbolizer attributes from the cache.
        // We are using raw symbolizer pointer as a cache key. As this
        // is not reliable, we also store symbolizer properties in the cache
        // and compare the actual value of properties in case of hit.
        markers_symbolizer const* attr_key = &sym_;

        // Limit the scope of the metrics mutex
        {
#ifdef MAPNIK_THREADSAFE
            std::lock_guard<std::mutex> lock(mutex_);
#endif
            auto attr_it = cached_attributes_.find(attr_key);
            if (attr_it != cached_attributes_.end() &&
                attr_it->second.second == sym_.properties)
            {
                r_attributes = attr_it->second.first;
            }
        }

        METRIC_UNUSED auto t11 = renderer_context_.metrics_.measure_time("Agg_RMS_AttrCache_Post_Search"); /* TODO: Remove this */

        if (!r_attributes)
        {
            renderer_context_.metrics_.measure_add("Agg_PMS_AttrCache_Miss");
            svg_attribute_type s_attributes;
            r_attributes = std::make_shared<svg_attribute_type>(get_marker_attributes(*stock_vector_marker, s_attributes));

            // We can only cache the attributes using the given key if no expressions are used in properties.
            // Otherwise the expressions may refer to feature-specific values.
            bool cacheable = std::all_of(
                sym_.properties.begin(), sym_.properties.end(),
                [](symbolizer_base::cont_type::value_type const& key_prop) { return !is_expression(key_prop.second); }
            );
            if (cacheable)
            {
#ifdef MAPNIK_THREADSAFE
                std::lock_guard<std::mutex> lock(mutex_);
#endif
                if (cached_attributes_.size() > attributes_cache_size)
                {
                    cached_attributes_.erase(cached_attributes_.begin());
                }
                cached_attributes_[attr_key] = std::make_pair(r_attributes, sym_.properties);
            }
        }

        if (filename_ != "shape://ellipse" ||
            !((has_key(sym_,keys::width) || has_key(sym_,keys::height))))
        {
            box2d<double> const& bbox = mark.bounding_box();
            setup_transform_scaling(image_tr, bbox.width(), bbox.height(), feature_, common_.vars_, sym_);
        }
        else
        {
            // special case for simple ellipse markers
            // to allow for full control over rx/ry dimensions
            // Ellipses are built procedurally. We do caching of the built ellipses, this is useful for rendering stages
            std::tuple<double, double, double> marker_key;
            {
                marker_key = std::tuple<double, double, double>(
                    get<double>(sym_, keys::width, feature_, common_.vars_, -std::numeric_limits<double>::infinity()),
                    get<double>(sym_, keys::height, feature_, common_.vars_, -std::numeric_limits<double>::infinity()),
                    get<double>(sym_, keys::stroke_width, feature_, common_.vars_, -std::numeric_limits<double>::infinity())
                );
            }

            {
                marker_ptr = nullptr;
#ifdef MAPNIK_THREADSAFE
                std::lock_guard<std::mutex> lock(mutex_);
#endif
                auto marker_it = cached_ellipses_.find(marker_key);
                if (marker_it != cached_ellipses_.end())
                {
                    marker_ptr = marker_it->second;
                }
            }

            if (!marker_ptr)
            {
                marker_ptr = std::make_shared<svg_storage_type>();
                vertex_stl_adapter<svg_path_storage> stl_storage(marker_ptr->source());
                svg_path_adapter svg_path(stl_storage);
                build_ellipse(sym_, feature_, common_.vars_, *marker_ptr, svg_path);

#ifdef MAPNIK_THREADSAFE
                std::lock_guard<std::mutex> lock(mutex_);
#endif
                if (cached_ellipses_.size() > ellipses_cache_size)
                {
                    cached_ellipses_.erase(cached_ellipses_.begin());
                }
                cached_ellipses_.emplace(marker_key, marker_ptr);

            }
        }

        vertex_stl_adapter<svg_path_storage> stl_storage(marker_ptr->source());
        svg_path_adapter svg_path(stl_storage);

        if (auto image_transform = get_optional<transform_type>(sym_, keys::image_transform))
        {
            evaluate_transform(image_tr, feature_, common_.vars_, *image_transform, common_.scale_factor_);
        }

        vector_dispatch_type rasterizer_dispatch(marker_ptr,
                                                 svg_path,
                                                 r_attributes,
                                                 image_tr,
                                                 sym_,
                                                 *common_.detector_,
                                                 common_.scale_factor_,
                                                 feature_,
                                                 common_.vars_,
                                                 snap_to_pixels,
                                                 renderer_context_);
        render_marker(mark, rasterizer_dispatch);
    }

    void operator() (marker_rgba8 const& mark) const
    {
        METRIC_UNUSED auto t = renderer_context_.metrics_.measure_time("Agg_RMS_visit_rgba8"); /* TODO: Remove this */
        agg::trans_affine image_tr = agg::trans_affine_scaling(common_.scale_factor_);

        setup_transform_scaling(image_tr, mark.width(), mark.height(), feature_, common_.vars_, sym_);
        auto image_transform = get_optional<transform_type>(sym_, keys::image_transform);
        if (image_transform) evaluate_transform(image_tr, feature_, common_.vars_, *image_transform, common_.scale_factor_);
        box2d<double> const& bbox = mark.bounding_box();
        mapnik::image_rgba8 const& marker = mark.get_data();
        // - clamp sizes to > 4 pixels of interactivity
        coord2d center = bbox.center();
        agg::trans_affine_translation recenter(-center.x, -center.y);
        agg::trans_affine marker_trans = recenter * image_tr;
        raster_dispatch_type rasterizer_dispatch(marker,
                                                 marker_trans,
                                                 sym_,
                                                 *common_.detector_,
                                                 common_.scale_factor_,
                                                 feature_,
                                                 common_.vars_,
                                                 renderer_context_);

        render_marker(mark, rasterizer_dispatch);
    }

  private:
    std::string const& filename_;
    markers_symbolizer const& sym_;
    mapnik::feature_impl & feature_;
    proj_transform const& prj_trans_;
    RendererType const& common_;
    box2d<double> const& clip_box_;
    ContextType & renderer_context_;

#ifdef MAPNIK_THREADSAFE
    static std::mutex mutex_;
#endif

    static std::map<
               markers_symbolizer const*,
               std::pair<svg_attribute_ptr, markers_symbolizer::cont_type>
           > cached_attributes_;
    static std::map<
               std::tuple<double, double, double>,
               svg_path_ptr
           > cached_ellipses_;

    static constexpr size_t attributes_cache_size = 256; // maximum number attributes to cache
    static constexpr size_t ellipses_cache_size = 256; // maximum number ellipses to cache
};

#ifdef MAPNIK_THREADSAFE
template <typename Detector, typename RendererType, typename ContextType>
std::mutex render_marker_symbolizer_visitor<Detector, RendererType, ContextType>::mutex_;
#endif

template <typename Detector, typename RendererType, typename ContextType>
std::map<
    markers_symbolizer const*,
    std::pair<svg_attribute_ptr, markers_symbolizer::cont_type>
> render_marker_symbolizer_visitor<Detector, RendererType, ContextType>::cached_attributes_;

template <typename Detector, typename RendererType, typename ContextType>
std::map<
    std::tuple<double, double, double>,
    svg_path_ptr
> render_marker_symbolizer_visitor<Detector, RendererType, ContextType>::cached_ellipses_;

} // namespace detail

markers_dispatch_params::markers_dispatch_params(box2d<double> const& size,
                                                 agg::trans_affine const& tr,
                                                 symbolizer_base const& sym,
                                                 feature_impl const& feature,
                                                 attributes const& vars,
                                                 double scale,
                                                 bool snap)
    : placement_params{
        size,
        tr,
        get<value_double, keys::spacing>(sym, feature, vars),
        get<value_double, keys::max_error>(sym, feature, vars),
        get<value_bool, keys::allow_overlap>(sym, feature, vars),
        get<value_bool, keys::avoid_edges>(sym, feature, vars),
        get<direction_enum, keys::direction>(sym, feature, vars)}
    , placement_method(get<marker_placement_enum, keys::markers_placement_type>(sym, feature, vars))
    , ignore_placement(get<value_bool, keys::ignore_placement>(sym, feature, vars))
    , snap_to_pixels(snap)
    , scale_factor(scale)
    , opacity(get<value_double, keys::opacity>(sym, feature, vars))
{
    placement_params.spacing *= scale;
}

void render_markers_symbolizer(markers_symbolizer const& sym,
                               mapnik::feature_impl & feature,
                               proj_transform const& prj_trans,
                               renderer_common const& common,
                               box2d<double> const& clip_box,
                               markers_renderer_context & renderer_context)
{
    using Detector = label_collision_detector4;
    using RendererType = renderer_common;
    using ContextType = markers_renderer_context;
    using VisitorType = detail::render_marker_symbolizer_visitor<Detector,
                                                                 RendererType,
                                                                 ContextType>;
    METRIC_UNUSED auto t = renderer_context.metrics_.measure_time("Agg_RMS_"); /* TODO: Remove this */

    std::string filename = get<std::string>(sym, keys::file, feature, common.vars_, "shape://ellipse");
    if (!filename.empty())
    {
        auto mark = mapnik::marker_cache::instance().find(filename, true);
        VisitorType visitor(filename, sym, feature, prj_trans, common, clip_box,
                            renderer_context);
        util::apply_visitor(visitor, *mark);
    }
}

} // namespace mapnik
