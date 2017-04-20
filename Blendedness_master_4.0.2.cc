// -*- LSST-C++ -*-
/*
 * LSST Data Management System
 * Copyright 2015 LSST/AURA
 *
 * This product includes software developed by the
 * LSST Project (http://www.lsst.org/).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the LSST License Statement and
 * the GNU General Public License along with this program.  If not,
 * see <http://www.lsstcorp.org/LegalNotices/>.
 */

#include "boost/math/special_functions/erf.hpp"
#include <boost/math/constants/constants.hpp>

#include "lsst/utils/ieee.h"
#include "lsst/meas/algorithms/Blendedness.h"
#include "lsst/afw/detection/HeavyFootprint.h"
#include "lsst/afw/geom/ellipses/Ellipse.h"
#include "lsst/afw/geom/ellipses/PixelRegion.h"
#include "lsst/afw/geom/ellipses/GridTransform.h"

namespace lsst { namespace meas { namespace algorithms {


namespace {


double computeOldBlendedness(
    PTR(afw::detection::Footprint const) childFootprint,
    afw::image::Image<float> const & parentImage
) {
    if (!childFootprint) {
        throw LSST_EXCEPT(
            pex::exceptions::LogicErrorException,
            "blendedness.old requires a Footprint."
        );
    }

    PTR(afw::detection::HeavyFootprint<float> const) childHeavy =
        boost::dynamic_pointer_cast<afw::detection::HeavyFootprint<float> const>(childFootprint);

    if (!childHeavy) {
        return 0.0;  // if it's not a HeavyFootprint, it's not blended.
    }

    if (!parentImage.getBBox(afw::image::PARENT).contains(childHeavy->getBBox())) {
        throw LSST_EXCEPT(
            pex::exceptions::LogicErrorException,
            "Child footprint extends beyond image."
        );
    }

    // Iterate over all the spans in the child HeavyFootprint,
    // along with iterators for the child pixels (from the HeavyFootprint)
    // and parent pixels (from the Exposure).
    typedef afw::detection::Footprint::SpanList::const_iterator SpanIter;
    typedef afw::image::Image<float>::const_x_iterator ParentPixIter;
    typedef ndarray::Array<float const,1,1>::Iterator ChildPixIter;
    SpanIter spanIter = childHeavy->getSpans().begin();
    SpanIter const spanEnd = childHeavy->getSpans().end();
    ChildPixIter childPixIter = childHeavy->getImageArray().begin();
    double cp = 0.0;   // child.dot(parent)
    double cc = 0.0;   // child.dot(child)
    while (spanIter != spanEnd) {
        afw::geom::Span const & span = **spanIter;
        ParentPixIter parentPixIter = parentImage.x_at(
            span.getBeginX() - parentImage.getX0(),
            span.getY() - parentImage.getY0()
        );
        int const width = span.getWidth();
        // Iterate over the pixels within the span, updating the dot products.
        for (int x = 0; x < width; ++parentPixIter, ++childPixIter, ++x) {
            cp += (*childPixIter) * ((*parentPixIter) - (*childPixIter));
            cc += (*childPixIter) * (*childPixIter);
        }
        ++spanIter;
    }
    if (cc > 0.0) {
        return cp/cc;
    }
    return 0.0;
}


class FluxAccumulator {
public:

    FluxAccumulator() : _w(0.0), _ww(0.0), _wd(0.0) {}

    void operator()(double, double, float weight, float data) {
        _w += weight;
        _ww += weight*weight;
        _wd += weight*data;
    }

    double getFlux() const { return _w*_wd/_ww; }

protected:
    double _w;
    double _ww;
    double _wd;
};


class ShapeAccumulator : public FluxAccumulator {
public:

    ShapeAccumulator() : FluxAccumulator(), _wdxx(0.0), _wdyy(0.0), _wdxy(0.0) {}

    void operator()(double x, double y, float weight, float data) {
        FluxAccumulator::operator()(x, y, weight, data);
        _wdxx += x*x*weight*data;
        _wdyy += y*y*weight*data;
        _wdxy += x*y*weight*data;
    }

    afw::geom::ellipses::Quadrupole getShape() const {
        // Factor of 2 corrects for bias from weight function (correct is exact for an object
        // with a Gaussian profile.)
        return afw::geom::ellipses::Quadrupole(2.0*_wdxx/_wd, 2.0*_wdyy/_wd, 2.0*_wdxy/_wd);
    }

private:
    double _wdxx;
    double _wdyy;
    double _wdxy;
};


template <typename Accumulator>
void computeMoments(
    afw::image::MaskedImage<float> const & image,
    afw::geom::Point2D const & centroid,
    afw::geom::ellipses::Quadrupole const & shape,
    double nSigmaWeightMax,
    Accumulator & accumulatorRaw,
    Accumulator & accumulatorAbs
) {
    afw::geom::Box2I bbox = image.getBBox(lsst::afw::image::PARENT);

    afw::geom::ellipses::Ellipse ellipse(shape, centroid);
    ellipse.getCore().scale(nSigmaWeightMax);

    // To evaluate an elliptically-symmetric function, we transform points
    // by the following transform, then evaluate a circularly-symmetric function
    // at the transformed positions.
    afw::geom::LinearTransform transform = shape.getGridTransform();

    typedef afw::geom::ellipses::PixelRegion::Iterator SpanIter;    // yields Spans
    typedef afw::geom::Span::Iterator PointIter;                    // yields Point2I positions
    typedef afw::image::MaskedImage<float>::const_x_iterator PixelIter;   // yields pixel values

    afw::geom::ellipses::PixelRegion region(ellipse);
    bool isContained = bbox.contains(region.getBBox());
    SpanIter const spanEnd = region.end();
    for (SpanIter spanIter = region.begin(); spanIter != spanEnd; ++spanIter) {
        afw::geom::Span span = *spanIter;
        if (!isContained) {
            if (span.getY() < bbox.getMinY() || span.getY() > bbox.getMaxY()) {
                continue;
            }
            span = afw::geom::Span(
                span.getY(),
                std::max(span.getMinX(), bbox.getMinX()),
                std::min(span.getMaxX(), bbox.getMaxX())
            );
            if (span.getMinX() > span.getMaxX()) {
                continue;
            }
        }
        PixelIter pixelIter = image.x_at(
            span.getBeginX() - image.getX0(),
            span.getY() - image.getY0()
        );
        PointIter const pointEnd = span.end();
        for (PointIter pointIter = span.begin(); pointIter != pointEnd; ++pointIter, ++pixelIter) {
            afw::geom::Extent2D d = afw::geom::Point2D(*pointIter) - centroid;
            afw::geom::Extent2D td = transform(d);
            // use single precision for faster exp, erf
            float weight = std::exp(static_cast<float>(-0.5*td.computeSquaredNorm()));
            float data = pixelIter.image();
            accumulatorRaw(d.getX(), d.getY(), weight, data);
            float variance = pixelIter.variance();
            float mu = (std::sqrt(variance/(2.0f/boost::math::constants::pi<float>()))*
                        std::exp(-0.5f*(data*data)/variance)) +
                0.5f*data*boost::math::erfc(-data/std::sqrt(2.0f*variance));
            float bias = (std::sqrt(2.0f*variance/boost::math::constants::pi<float>())*
                          std::exp(-0.5f*(mu*mu)/variance)) -
                mu*boost::math::erfc(mu/std::sqrt(2.0f*variance));
            accumulatorAbs(d.getX(), d.getY(), weight, std::max(std::abs(data) - bias, 0.0f));
        }
    }
}


} // anonymous


Blendedness::Blendedness(BlendednessControl const & ctrl, afw::table::Schema & schema) :
    _ctrl(ctrl)
{
    if (_ctrl.doOld) {
        _old = schema.addField<double>(
            "blendedness.old",
            "blendedness from dot products: (child.dot(parent)/child.dot(child) - 1)"
        );
    }
    if (_ctrl.doFlux) {
        _fluxRaw = schema.addField<double>(
            "blendedness.raw.flux",
            "measure of how flux is affected by neighbors: (1 - flux.child/flux.parent)"
        );
        _fluxChildRaw = schema.addField<double>(
            "blendedness.raw.flux.child",
            "flux of the child, measured with a Gaussian weight matched to the child",
            "dn"
        );
        _fluxParentRaw = schema.addField<double>(
            "blendedness.raw.flux.parent",
            "flux of the parent, measured with a Gaussian weight matched to the child",
            "dn"
        );
        _fluxAbs = schema.addField<double>(
            "blendedness.abs.flux",
            "measure of how flux is affected by neighbors: (1 - flux.child/flux.parent)"
            " (uses absolute values of pixels)"
        );
        _fluxChildAbs = schema.addField<double>(
            "blendedness.abs.flux.child",
            "flux of the child, measured with a Gaussian weight matched to the child"
            " (uses absolute values of pixels)",
            "dn"
        );
        _fluxParentAbs = schema.addField<double>(
            "blendedness.abs.flux.parent",
            "flux of the parent, measured with a Gaussian weight matched to the child"
            " (uses absolute values of pixels)",
            "dn"
        );
    }
    if (_ctrl.doShape) {
        _shapeChildRaw = schema.addField< afw::table::Moments<double> >(
            "blendedness.raw.shape.child",
            "shape of the child, measured with a Gaussian weight matched to the child",
            "dn"
        );
        _shapeParentRaw = schema.addField< afw::table::Moments<double> >(
            "blendedness.raw.shape.parent",
            "shape of the parent, measured with a Gaussian weight matched to the child",
            "dn"
        );
        _shapeChildAbs = schema.addField< afw::table::Moments<double> >(
            "blendedness.abs.shape.child",
            "shape of the child, measured with a Gaussian weight matched to the child"
            " (uses absolute values of pixels)",
            "dn"
        );
        _shapeParentAbs = schema.addField< afw::table::Moments<double> >(
            "blendedness.abs.shape.parent",
            "shape of the parent, measured with a Gaussian weight matched to the child"
            " (uses absolute values of pixels)",
            "dn"
        );
    }
    if (_ctrl.doShape || _ctrl.doFlux) {
        _flagGeneral = schema.addField<afw::table::Flag>(
            "blendedness.flags",
            "flag set if could not be measured because a required input was missing"
        );
        _flagNoCentroid = schema.addField<afw::table::Flag>(
            "blendedness.flags.noCentroid",
            "blendedness measurement was compromised by a bad centroid measurement"
        );
        _flagNoShape = schema.addField<afw::table::Flag>(
            "blendedness.flags.noShape",
            "blendedness measurement was compromised by a bad shape measurement"
        );
    }
}


void Blendedness::_measureMoments(
    afw::image::MaskedImage<float> const & image,
    afw::table::SourceRecord & child,
    afw::table::Key<double> const & fluxRawKey,
    afw::table::Key<double> const & fluxAbsKey,
    afw::table::Key< afw::table::Moments<double> > const & shapeRawKey,
    afw::table::Key< afw::table::Moments<double> > const & shapeAbsKey
) const {
    // TODO: check flags on centroid and shape, set flags on blendedness
    if (_ctrl.doShape || _ctrl.doFlux) {
        bool fatal = false;
        if (child.getCentroidFlag()) {
            child.set(_flagNoCentroid, true);
            // don't set general flag, because even a failed centroid should
            // just fall back to the peak, and that should be fine for this
            // measurement.
        }
        if (child.getShapeFlag()) {
            child.set(_flagNoShape, true);
            child.set(_flagGeneral, true);
        }
        if (!(child.getShape().getDeterminant() >= 0.0)) {
            // shape flag should have been set already, but we're paranoid
            child.set(_flagNoShape, true);
            child.set(_flagGeneral, true);
            fatal = true;
        }
        if (!(utils::isfinite(child.getX()) && utils::isfinite(child.getY()))) {
            // centroid flag should have been set already, but we're paranoid
            child.set(_flagNoCentroid, true);
            child.set(_flagGeneral, true);
            fatal = true;
        }
        if (fatal) return;
    }
    if (_ctrl.doShape) {
        ShapeAccumulator accumulatorRaw;
        ShapeAccumulator accumulatorAbs;
        computeMoments(
            image,
            child.getCentroid(),
            child.getShape(),
            _ctrl.nSigmaWeightMax,
            accumulatorRaw,
            accumulatorAbs
        );
        if (_ctrl.doFlux) {
            child.set(fluxRawKey, accumulatorRaw.getFlux());
            child.set(fluxAbsKey, accumulatorAbs.getFlux());
        }
        child.set(shapeRawKey, accumulatorRaw.getShape());
        child.set(shapeAbsKey, accumulatorAbs.getShape());
    } else if (_ctrl.doFlux) {
        FluxAccumulator accumulatorRaw;
        FluxAccumulator accumulatorAbs;
        computeMoments(
            image,
            child.getCentroid(),
            child.getShape(),
            _ctrl.nSigmaWeightMax,
            accumulatorRaw,
            accumulatorAbs
        );
        child.set(fluxRawKey, accumulatorRaw.getFlux());
        child.set(fluxAbsKey, accumulatorAbs.getFlux());
    }
}



void Blendedness::measureChildPixels(
    afw::image::MaskedImage<float> const & image,
    afw::table::SourceRecord & child
) const {
    _measureMoments(image, child, _fluxChildRaw, _fluxChildAbs, _shapeChildRaw, _shapeChildAbs);
}


void Blendedness::measureParentPixels(
    afw::image::MaskedImage<float> const & image,
    afw::table::SourceRecord & child
) const {
    if (_ctrl.doOld) {
        child.set(_old, computeOldBlendedness(child.getFootprint(), *image.getImage()));
    }
    _measureMoments(image, child, _fluxParentRaw, _fluxParentAbs, _shapeParentRaw,_shapeParentAbs);
    if (_ctrl.doFlux) {
        child.set(_fluxRaw, 1.0 - child.get(_fluxChildRaw)/child.get(_fluxParentRaw));
        child.set(_fluxAbs, 1.0 - child.get(_fluxChildAbs)/child.get(_fluxParentAbs));
        if (child.get(_fluxParentAbs) == 0.0) {
            // We can get NaNs in the absolute measure if both parent and child have only negative
            // biased-corrected fluxes (which we clip to zero).  We can't really recover from this,
            // so we should set the flag.
            child.set(_flagGeneral, true);
        }
    }
}


}}} // namespace lsst::meas::algorithms
