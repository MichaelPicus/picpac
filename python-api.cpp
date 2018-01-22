#include <fstream>
#include <boost/ref.hpp>
#include <boost/python.hpp>
#include <boost/python/make_constructor.hpp>
#include <boost/python/raw_function.hpp>
//#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/ndarrayobject.h>
#include "picpac.h"
#include "picpac-cv.h"
#include "Non-Maximum-Suppression/nms.h"
using namespace boost::python;
using namespace picpac;

namespace {

template <typename T>
T *get_ndarray_data (object &o) {
    PyArrayObject *nd = reinterpret_cast<PyArrayObject *>(o.ptr());
    return reinterpret_cast<T*>(PyArray_DATA(nd));
}

size_t get_ndarray_nbytes (object &o) {
    PyArrayObject *nd = reinterpret_cast<PyArrayObject *>(o.ptr());
    return size_t(PyArray_NBYTES(nd));
}

class SSDImageStream: public ImageStream {
public:
    SSDImageStream (std::string const &path, Config const &c)
        : ImageStream(fs::path(path), c) {
    }
    tuple next () {
        Value v(ImageStream::next());

        npy_intp images_dims[] = {1, v.image.rows, v.image.cols, v.image.channels()};
        npy_intp labels_dims[] = {1, v.label_size.height, v.label_size.width, ImageLoader::config.boxes.size()};
        npy_intp shift_dims[] = {1, v.label_size.height, v.label_size.width, ImageLoader::config.boxes.size() * 4};
        //next_shape(&images_dims, &labels_dims);
        object images = object(boost::python::handle<>(PyArray_SimpleNew(4, &images_dims[0], NPY_FLOAT)));
        CHECK(images.ptr());
        float *images_buf = get_ndarray_data<float>(images);

        object labels = object(boost::python::handle<>(PyArray_SimpleNew(4, &labels_dims[0], NPY_FLOAT)));
        CHECK(labels.ptr());
        float *labels_buf = get_ndarray_data<float>(labels);
        std::copy(v.labels.begin(), v.labels.end(), labels_buf);

        object shift = object(boost::python::handle<>(PyArray_SimpleNew(4, &shift_dims[0], NPY_FLOAT)));
        CHECK(shift.ptr());
        float *shift_buf = get_ndarray_data<float>(shift);
        std::copy(v.shift.begin(), v.shift.end(), shift_buf);

        object mask = object(boost::python::handle<>(PyArray_SimpleNew(4, &shift_dims[0], NPY_FLOAT)));
        CHECK(mask.ptr());
        float *mask_buf = get_ndarray_data<float>(mask);
        std::copy(v.mask.begin(), v.mask.end(), mask_buf);

        cv::Scalar mean(ImageLoader::config.mean_color1, ImageLoader::config.mean_color2, ImageLoader::config.mean_color3);
        impl::copy<float>(v.image, images_buf, mean, ImageLoader::config.bgr2rgb);



        return make_tuple(v.matched_boxes, images, labels, shift, mask);
    }
};

object create_image_stream (tuple args, dict kwargs) {
    object self = args[0];
    CHECK(len(args) > 1);
    string path = extract<string>(args[1]);
    SSDImageStream::Config config;
    /*
    bool train = extract<bool>(kwargs.get("train", true));
    unsigned K = extract<unsigned>(kwargs.get("K", 1));
    unsigned fold = extract<unsigned>(kwargs.get("fold", 0));
    if (K <= 1) {
        if (!train) {
            config.loop = false;
            config.reshuffle = false;
        }
    }
    else {
        config.kfold(K, fold, train);
    }
    */
#define PICPAC_CONFIG_UPDATE(C, P) \
    C.P = extract<decltype(C.P)>(kwargs.get(#P, C.P)) 
    PICPAC_CONFIG_UPDATE_ALL(config);
#undef PICPAC_CONFIG_UPDATE
    list boxes = extract<list>(kwargs.get("boxes"));
    for (int i = 0; i < len(boxes); ++i) {
        tuple b = extract<tuple>(boxes[i]);
        config.boxes.emplace_back(extract<float>(b[0]), extract<float>(b[1]));
    }

    return self.attr("__init__")(path, config);
};

object return_iterator (tuple args, dict kwargs) {
    object self = args[0];
    self.attr("reset")();
    return self;
};


void translate_eos (EoS const &)
{
    // Use the Python 'C' API to set up an exception object
    PyErr_SetNone(PyExc_StopIteration);
}

class SSDDetector {
    int downsize;
    float th;
    float nms_th;
    vector<cv::Size_<float>> dsize;
public:
    SSDDetector (list boxes, int downsize_, float th_, float nms_th_): downsize(downsize_), th(th_), nms_th(nms_th_) {
        for (int i = 0; i < len(boxes); ++i) {
            tuple b = extract<tuple>(boxes[i]);
            dsize.emplace_back(extract<float>(b[0])/downsize, extract<float>(b[1])/downsize);
        }
    }

    static float rect_score (float th, cv::Mat roi) {
        float sum = 0;
        for (int y = 0; y < roi.rows; ++y){
            float const *p = roi.ptr<float const>(y);
            for (int x = 0; x < roi.cols; ++x) {
                sum += p[x];
                if (p[x] < th) {
                    sum -= th - p[x];
                }
            }
        }
        return sum;
    }


    list apply (PyObject *prob_, PyObject *shifts_, PyObject *weights_) {
        PyArrayObject *prob = (PyArrayObject *)prob_;
        PyArrayObject *shifts = (PyArrayObject *)shifts_;
        PyArrayObject *weights = (PyArrayObject *)weights_;
        CHECK(PyArray_ISCONTIGUOUS(prob));
        CHECK(PyArray_ISCONTIGUOUS(shifts));
        CHECK(prob->nd == 4);
        CHECK(shifts->nd == 4);
        CHECK(prob->dimensions[0] == 1);
        int rows = prob->dimensions[1];
        int cols = prob->dimensions[2];
        CHECK(prob->dimensions[3] == dsize.size() * 2);
        CHECK(shifts->dimensions[0] == 1);
        CHECK(shifts->dimensions[1] == rows);
        CHECK(shifts->dimensions[2] == cols);
        CHECK(shifts->dimensions[3] == dsize.size() * 4);

        CHECK(weights->nd == 2);
        CHECK(weights->dimensions[0] == rows);
        CHECK(weights->dimensions[1] == cols);

        float *p = (float *)PyArray_DATA(prob);
        float *s = (float *)PyArray_DATA(shifts);
        cv::Mat W(rows, cols, CV_32F, PyArray_DATA(weights));


        vector<cv::Rect> rects;
        vector<float> scores;
        /*
        auto dsize1 = dsize;
        dsize1.pop_back();
        */
        vector<int> cnts(dsize.size(), 0);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                for (int k = 0; k < dsize.size(); ++k) {
                    if (p[1] > th) {
                        cnts[k] += 1;
                    }
                    p += 2;
                    s += 4;
                }
            }
        }
        int best = 0;
        for (int k = 1; k < dsize.size(); ++k) {
            if (cnts[k] > cnts[best]) {
                best = k;
            }
        }

        vector<bool> pick(dsize.size(), false);
        pick[best] = true;
        /*
        if (best > 0) pick[best-1] = true;
        if (best + 1 < pick.size()) pick[best+1]= true;
        */

        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                for (int k = 0; k < dsize.size(); ++k) {
                    auto const &bb = dsize[k];
                    if ((p[1] > th) && pick[k]) {
                        float w = bb.width + s[2];
                        float h = bb.height + s[3];
                        int x0 = int(round(x - w/2 + s[0]));
                        int y0 = int(round(y - h/2 + s[1]));
                        int w0 = int(round(w));
                        int h0 = int(round(h));
                        if (x0 < 0) {
                            w0 += x0;
                            x0 = 0;
                        }
                        if (y0 < 0) {
                            h0 += y0;
                            y0 = 0;
                        }
                        if (x0 + w0 > cols) {
                            w0 -= x0 + w0 - cols;
                        }
                        if (y0 + h0 > rows) {
                            h0 -= y0 + h0 - rows;
                        }
                        if (w0 > 0 && h0 > 0) {
                            rects.emplace_back(x0, y0, w0, h0);
                            scores.emplace_back(rect_score(th, W(rects.back())));
                        }
                    }
                    p += 2;
                    s += 4;
                }
            }
        }
#if 0
        vector<cv::Rect> res(rects);
#else
        vector<cv::Rect> res;
        nms2(rects, scores, res, nms_th);
#endif
        list r;

        
        for (auto const &b: res) {
            r.append(make_tuple(b.x, b.y, b.width, b.height));
        }
        return r;
    }
};

}

BOOST_PYTHON_MODULE(picpac_ssd)
{
    scope().attr("__doc__") = "PicPoc Python API";
    register_exception_translator<EoS>(&translate_eos);
    class_<SSDImageStream::Config>("ImageStreamParams", init<>());
    class_<SSDImageStream, boost::noncopyable>("ImageStream", no_init)
        .def("__init__", raw_function(create_image_stream), "exposed ctor")
        .def("__iter__", raw_function(return_iterator))
        .def(init<string, SSDImageStream::Config const&>()) // C++ constructor not exposed
        .def("next", &SSDImageStream::next)
        .def("size", &SSDImageStream::size)
        .def("reset", &SSDImageStream::reset)
        .def("categories", &SSDImageStream::categories)
    ;
    class_<SSDDetector, boost::noncopyable>("Detector", init<list, int,float, float>() )
        .def("apply", &SSDDetector::apply)
    ;
#undef NUMPY_IMPORT_ARRAY_RETVAL
#define NUMPY_IMPORT_ARRAY_RETVAL
    import_array();
}

