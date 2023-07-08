#ifndef V4L2_CONVERTER_H_
#define V4L2_CONVERTER_H_

#include <functional>
#include <mutex>
#include <queue>

// Linux
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

// WebRTC
#include <api/scoped_refptr.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame_type.h>
#include <modules/video_coding/include/video_error_codes.h>
#include <rtc_base/logging.h>
#include <rtc_base/platform_thread.h>
#include <rtc_base/time_utils.h>
#include <third_party/libyuv/include/libyuv.h>

#include "v4l2_native_buffer.h"

template <class T>
class ConcurrentQueue {
 public:
  void push(T t) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(t);
  }
  std::optional<T> pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    T t = queue_.front();
    queue_.pop();
    return t;
  }
  bool empty() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }
  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_ = std::queue<T>();
  }

 private:
  std::queue<T> queue_;
  std::mutex mutex_;
};

class V4L2Runner {
 public:
  ~V4L2Runner() {
    abort_poll_ = true;
    thread_.Finalize();
  }

  static std::shared_ptr<V4L2Runner> Create(int fd,
                                            int src_count,
                                            int src_memory,
                                            int dst_memory) {
    auto p = std::make_shared<V4L2Runner>();
    p->fd_ = fd;
    p->src_count_ = src_count;
    p->src_memory_ = src_memory;
    p->dst_memory_ = dst_memory;
    p->abort_poll_ = false;
    p->thread_ = rtc::PlatformThread::SpawnJoinable(
        [p = p.get()]() { p->PollProcess(); }, "PollThread",
        rtc::ThreadAttributes().SetPriority(rtc::ThreadPriority::kHigh));
    for (int i = 0; i < src_count; i++) {
      p->output_buffers_available_.push(i);
    }
    return p;
  }

  typedef std::function<void(v4l2_buffer*, std::function<void()>)>
      OnCompleteCallback;

  int Enqueue(v4l2_buffer* v4l2_buf, OnCompleteCallback on_complete) {
    if (ioctl(fd_, VIDIOC_QBUF, v4l2_buf) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to queue output buffer";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    on_completes_.push(on_complete);

    return WEBRTC_VIDEO_CODEC_OK;
  }

  std::optional<int> PopAvailableBufferIndex() {
    return output_buffers_available_.pop();
  }

 private:
  void PollProcess() {
    while (true) {
      pollfd p = {fd_, POLLIN, 0};
      int ret = poll(&p, 1, 200);
      if (abort_poll_ && output_buffers_available_.size() == src_count_) {
        break;
      }
      if (ret == -1) {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "  unexpected error"
                          << "  ret: " << ret;
        break;
      }
      if (p.revents & POLLIN) {
        v4l2_buffer v4l2_buf = {};
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        v4l2_buf.memory = src_memory_;
        v4l2_buf.length = 1;
        v4l2_plane planes[VIDEO_MAX_PLANES] = {};
        v4l2_buf.m.planes = planes;
        int ret = ioctl(fd_, VIDIOC_DQBUF, &v4l2_buf);
        if (ret != 0) {
          RTC_LOG(LS_ERROR)
              << "Failed to dequeue output buffer: error=" << strerror(errno);
        } else {
          output_buffers_available_.push(v4l2_buf.index);
        }

        v4l2_buf = {};
        memset(planes, 0, sizeof(planes));
        v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;
        v4l2_buf.length = 1;
        v4l2_buf.m.planes = planes;
        ret = ioctl(fd_, VIDIOC_DQBUF, &v4l2_buf);
        if (ret != 0) {
          RTC_LOG(LS_ERROR)
              << "Failed to dequeue capture buffer: error=" << strerror(errno);
        } else {
          std::optional<OnCompleteCallback> on_complete = on_completes_.pop();
          if (!on_complete) {
            RTC_LOG(LS_ERROR) << "on_completes_ is empty.";
          } else {
            (*on_complete)(&v4l2_buf, [fd = fd_, v4l2_buf]() mutable {
              v4l2_plane planes[VIDEO_MAX_PLANES] = {};
              v4l2_buf.m.planes = planes;
              if (ioctl(fd, VIDIOC_QBUF, &v4l2_buf) < 0) {
                RTC_LOG(LS_ERROR) << "Failed to enqueue capture buffer: error="
                                  << strerror(errno);
              }
            });
          }
        }
      }
    }
  }

 private:
  int fd_;
  int src_count_;
  int src_memory_;
  int dst_memory_;

  ConcurrentQueue<int> output_buffers_available_;
  ConcurrentQueue<OnCompleteCallback> on_completes_;
  std::atomic<bool> abort_poll_;
  rtc::PlatformThread thread_;
};

// MMAP なバッファと DMABUF なバッファを扱うためのクラス
class V4L2Buffers {
 public:
  struct PlaneBuffer {
    void* start = nullptr;
    size_t length = 0;
    int sizeimage = 0;
    int bytesperline = 0;
    int fd = 0;
  };
  struct Buffer {
    PlaneBuffer planes[VIDEO_MAX_PLANES];
    size_t n_planes = 0;
  };

  int Allocate(int fd,
               int type,
               int memory,
               int req_count,
               v4l2_format* format,
               bool export_dmafds) {
    Deallocate();

    v4l2_requestbuffers reqbufs = {};
    reqbufs.count = req_count;
    reqbufs.type = type;
    reqbufs.memory = memory;
    if (ioctl(fd, VIDIOC_REQBUFS, &reqbufs) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to request buffers";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    buffers_.resize(reqbufs.count);
    RTC_LOG(LS_INFO) << "Request buffers: type=" << reqbufs.type
                     << " memory=" << reqbufs.memory
                     << " count=" << reqbufs.count
                     << " capabilities=" << reqbufs.capabilities;

    if (memory == V4L2_MEMORY_MMAP) {
      for (unsigned int i = 0; i < reqbufs.count; i++) {
        v4l2_plane planes[VIDEO_MAX_PLANES];
        v4l2_buffer v4l2_buf = {};
        v4l2_buf.type = type;
        v4l2_buf.memory = V4L2_MEMORY_MMAP;
        v4l2_buf.index = i;
        v4l2_buf.length = 1;
        v4l2_buf.m.planes = planes;
        if (ioctl(fd, VIDIOC_QUERYBUF, &v4l2_buf) < 0) {
          RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to query buffer"
                            << "  index: " << i;
          return WEBRTC_VIDEO_CODEC_ERROR;
        }

        Buffer buffer;
        buffer.n_planes = v4l2_buf.length;
        for (size_t j = 0; j < buffer.n_planes; j++) {
          PlaneBuffer& plane = buffer.planes[j];
          if (!export_dmafds) {
            plane.start =
                mmap(0, v4l2_buf.m.planes[j].length, PROT_READ | PROT_WRITE,
                     MAP_SHARED, fd, v4l2_buf.m.planes[j].m.mem_offset);
            if (plane.start == MAP_FAILED) {
              RTC_LOG(LS_ERROR)
                  << __FUNCTION__ << "  Failed to map plane buffer"
                  << "  buffer index: " << i << "  plane index: " << j;
              return WEBRTC_VIDEO_CODEC_ERROR;
            }
            plane.length = v4l2_buf.m.planes[j].length;
          } else {
            v4l2_exportbuffer expbuf = {};
            expbuf.type = type;
            expbuf.index = i;
            expbuf.plane = j;
            if (ioctl(fd, VIDIOC_EXPBUF, &expbuf) < 0) {
              RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to export buffer"
                                << " index=" << i << " plane=" << j
                                << " error=" << strerror(errno);
              return WEBRTC_VIDEO_CODEC_ERROR;
            }
            plane.fd = expbuf.fd;
          }
          plane.sizeimage = format->fmt.pix_mp.plane_fmt[j].sizeimage;
          plane.bytesperline = format->fmt.pix_mp.plane_fmt[j].bytesperline;
        }
        buffers_[i] = buffer;
      }
    }
    fd_ = fd;
    type_ = type;
    memory_ = memory;
    export_dmafds_ = export_dmafds;

    return WEBRTC_VIDEO_CODEC_OK;
  }

  void Deallocate() {
    for (int i = 0; i < buffers_.size(); i++) {
      Buffer& buffer = buffers_[i];
      for (size_t j = 0; j < buffer.n_planes; j++) {
        PlaneBuffer* plane = &buffer.planes[j];
        if (plane->start != nullptr) {
          if (munmap(plane->start, plane->length) < 0) {
            RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to unmap buffer"
                              << "  index: " << i;
          }
        }
        if (plane->fd != 0) {
          close(plane->fd);
        }
      }
    }
    buffers_.clear();

    if (fd_ != 0) {
      v4l2_requestbuffers reqbufs = {};
      reqbufs.count = 0;
      reqbufs.type = type_;
      reqbufs.memory = memory_;
      if (ioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to free buffers: error="
                          << strerror(errno);
      }
      fd_ = 0;
      type_ = 0;
      memory_ = 0;
    }
  }

  ~V4L2Buffers() { Deallocate(); }

  int type() const { return type_; }
  int memory() const { return memory_; }
  int count() const { return buffers_.size(); }
  bool dmafds_exported() const { return export_dmafds_; }
  Buffer& at(int index) { return buffers_.at(index); }

 private:
  int fd_ = 0;
  int type_ = 0;
  int memory_ = 0;
  bool export_dmafds_ = false;
  std::vector<Buffer> buffers_;
};

class V4L2H264Converter {
 public:
  typedef std::function<void(uint8_t*, int, int64_t, bool)> OnCompleteCallback;

  static std::shared_ptr<V4L2H264Converter> Create(int src_memory,
                                                   int src_width,
                                                   int src_height,
                                                   int src_stride) {
    auto p = std::make_shared<V4L2H264Converter>();
    if (p->Init(src_memory, src_width, src_height, src_stride) !=
        WEBRTC_VIDEO_CODEC_OK) {
      return nullptr;
    }
    return p;
  }

 private:
  static constexpr int NUM_OUTPUT_BUFFERS = 4;
  static constexpr int NUM_CAPTURE_BUFFERS = 4;

  int Init(int src_memory, int src_width, int src_height, int src_stride) {
    const char device_name[] = "/dev/video11";
    fd_ = open(device_name, O_RDWR, 0);
    if (fd_ < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to create v4l2 encoder";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    v4l2_control ctrl = {};
    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
    ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to set profile";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
    ctrl.value = V4L2_MPEG_VIDEO_H264_LEVEL_4_2;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to set level";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    ctrl.value = 500;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to set intra period";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    ctrl.id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    ctrl.value = 1;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to enable inline header";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    v4l2_format src_fmt = {};
    src_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    src_fmt.fmt.pix_mp.width = src_width;
    src_fmt.fmt.pix_mp.height = src_height;
    src_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    src_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = src_stride;
    src_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    src_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;  // 意味ないらしい
    src_fmt.fmt.pix_mp.num_planes = 1;  // Plane 1 枚に詰め込むので 1
    if (ioctl(fd_, VIDIOC_S_FMT, &src_fmt) < 0) {
      RTC_LOG(LS_ERROR) << "Failed to set output format";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    RTC_LOG(LS_INFO) << __FUNCTION__ << "  Output buffer format"
                     << "  width:" << src_fmt.fmt.pix_mp.width
                     << "  height:" << src_fmt.fmt.pix_mp.height
                     << "  bytesperline:"
                     << src_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    v4l2_format dst_fmt = {};
    dst_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    dst_fmt.fmt.pix_mp.width = src_width;
    dst_fmt.fmt.pix_mp.height = src_height;
    dst_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264;
    dst_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    dst_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
    dst_fmt.fmt.pix_mp.num_planes = 1;
    dst_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
    dst_fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 512 << 10;
    if (ioctl(fd_, VIDIOC_S_FMT, &dst_fmt) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to set capture format";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    int r =
        src_buffers_.Allocate(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                              src_memory, NUM_OUTPUT_BUFFERS, &src_fmt, false);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      RTC_LOG(LS_ERROR) << __FUNCTION__
                        << "  Failed to allocate output buffers";
      return r;
    }

    r = dst_buffers_.Allocate(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                              V4L2_MEMORY_MMAP, NUM_CAPTURE_BUFFERS, &dst_fmt,
                              false);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to request output buffers";
      return r;
    }

    for (int i = 0; i < dst_buffers_.count(); i++) {
      v4l2_plane planes[VIDEO_MAX_PLANES];
      v4l2_buffer v4l2_buf = {};
      v4l2_buf.type = dst_buffers_.type();
      v4l2_buf.memory = dst_buffers_.memory();
      v4l2_buf.index = i;
      v4l2_buf.length = 1;
      v4l2_buf.m.planes = planes;
      if (ioctl(fd_, VIDIOC_QBUF, &v4l2_buf) < 0) {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to queue buffer"
                          << "  index=" << i << " error=" << strerror(errno);
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to start output stream";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to start capture stream";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    runner_ = V4L2Runner::Create(fd_, src_buffers_.count(), src_memory,
                                 V4L2_MEMORY_MMAP);

    return WEBRTC_VIDEO_CODEC_OK;
  }

 public:
  int fd() const { return fd_; }

  int Encode(const rtc::scoped_refptr<webrtc::VideoFrameBuffer>& frame_buffer,
             int64_t timestamp_us,
             bool force_key_frame,
             OnCompleteCallback on_complete) {
    if (force_key_frame) {
      v4l2_control ctrl = {};
      ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
      ctrl.value = 1;
      if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to request I frame";
      }
    }

    std::optional<int> index = runner_->PopAvailableBufferIndex();
    if (!index) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  No available output buffers";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    rtc::scoped_refptr<webrtc::VideoFrameBuffer> bind_buffer;
    v4l2_buffer v4l2_buf = {};
    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    v4l2_buf.index = *index;
    v4l2_buf.field = V4L2_FIELD_NONE;
    v4l2_buf.length = 1;
    v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    v4l2_buf.m.planes = planes;
    v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    v4l2_buf.timestamp.tv_sec = timestamp_us / rtc::kNumMicrosecsPerSec;
    v4l2_buf.timestamp.tv_usec = timestamp_us % rtc::kNumMicrosecsPerSec;
    if (frame_buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
      v4l2_buf.memory = V4L2_MEMORY_DMABUF;
      auto native_buffer = static_cast<V4L2NativeBuffer*>(frame_buffer.get());
      planes[0].m.fd = native_buffer->fd();
      planes[0].bytesused = native_buffer->size();
      planes[0].length = native_buffer->size();
      bind_buffer = frame_buffer;
    } else {
      v4l2_buf.memory = V4L2_MEMORY_MMAP;

      auto& src_buffer = src_buffers_.at(v4l2_buf.index);

      rtc::scoped_refptr<webrtc::I420BufferInterface> i420_buffer =
          frame_buffer->ToI420();
      int width = i420_buffer->width();
      int height = i420_buffer->height();
      int dst_stride = src_buffer.planes[0].bytesperline;
      int dst_chroma_stride = (dst_stride + 1) / 2;
      int dst_chroma_height = (height + 1) / 2;
      uint8_t* dst_y = (uint8_t*)src_buffer.planes[0].start;
      uint8_t* dst_u = dst_y + dst_stride * height;
      uint8_t* dst_v = dst_u + dst_chroma_stride * dst_chroma_height;
      libyuv::I420Copy(i420_buffer->DataY(), i420_buffer->StrideY(),
                       i420_buffer->DataU(), i420_buffer->StrideU(),
                       i420_buffer->DataV(), i420_buffer->StrideV(), dst_y,
                       dst_stride, dst_u, dst_chroma_stride, dst_v,
                       dst_chroma_stride, width, height);
      bind_buffer = i420_buffer;
    }

    runner_->Enqueue(
        &v4l2_buf, [this, bind_buffer, on_complete](
                       v4l2_buffer* v4l2_buf, std::function<void()> on_next) {
          int64_t timestamp_us =
              v4l2_buf->timestamp.tv_sec * rtc::kNumMicrosecsPerSec +
              v4l2_buf->timestamp.tv_usec;
          bool is_key_frame = !!(v4l2_buf->flags & V4L2_BUF_FLAG_KEYFRAME);
          V4L2Buffers::PlaneBuffer& plane =
              dst_buffers_.at(v4l2_buf->index).planes[0];
          on_complete((uint8_t*)plane.start, v4l2_buf->m.planes[0].bytesused,
                      timestamp_us, is_key_frame);
          on_next();
        });

    return WEBRTC_VIDEO_CODEC_OK;
  }

  ~V4L2H264Converter() {
    runner_.reset();

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to stop output stream";
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to stop capture stream";
    }

    src_buffers_.Deallocate();
    dst_buffers_.Deallocate();

    close(fd_);
  }

 private:
  int fd_ = 0;

  V4L2Buffers src_buffers_;
  V4L2Buffers dst_buffers_;

  std::shared_ptr<V4L2Runner> runner_;
};

class V4L2Scaler {
 public:
  typedef std::function<void(rtc::scoped_refptr<webrtc::VideoFrameBuffer>,
                             int64_t)>
      OnCompleteCallback;

  static std::shared_ptr<V4L2Scaler> Create(int src_memory,
                                            int src_width,
                                            int src_height,
                                            int src_stride,
                                            bool dst_export_dmafds,
                                            int dst_width,
                                            int dst_height,
                                            int dst_stride) {
    auto p = std::make_shared<V4L2Scaler>();
    if (p->Init(src_memory, src_width, src_height, src_stride,
                dst_export_dmafds, dst_width, dst_height,
                dst_stride) != WEBRTC_VIDEO_CODEC_OK) {
      return nullptr;
    }
    return p;
  }

 private:
  static constexpr int NUM_OUTPUT_BUFFERS = 4;
  static constexpr int NUM_CAPTURE_BUFFERS = 4;

  int Init(int src_memory,
           int src_width,
           int src_height,
           int src_stride,
           bool dst_export_dmafds,
           int dst_width,
           int dst_height,
           int dst_stride) {
    RTC_LOG(LS_INFO) << "V4L2Scaler::Init src_memory=" << src_memory
                     << " src_width=" << src_width
                     << " src_height=" << src_height
                     << " src_stride=" << src_stride
                     << " dst_export_dmafds=" << dst_export_dmafds
                     << " dst_width=" << dst_width
                     << " dst_height=" << dst_height
                     << " dst_stride=" << dst_stride;

    const char device_name[] = "/dev/video12";
    fd_ = open(device_name, O_RDWR, 0);
    if (fd_ < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to create v4l2 scaler";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    v4l2_format src_fmt = {};
    src_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    src_fmt.fmt.pix_mp.width = src_width;
    src_fmt.fmt.pix_mp.height = src_height;
    src_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    src_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = src_stride;
    src_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    src_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;  // 意味ないらしい
    src_fmt.fmt.pix_mp.num_planes = 1;  // Plane 1 枚に詰め込むので 1
    if (ioctl(fd_, VIDIOC_S_FMT, &src_fmt) < 0) {
      RTC_LOG(LS_ERROR) << "Failed to set output format";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    RTC_LOG(LS_INFO) << __FUNCTION__ << "  Output buffer format"
                     << "  width:" << src_fmt.fmt.pix_mp.width
                     << "  height:" << src_fmt.fmt.pix_mp.height
                     << "  bytesperline:"
                     << src_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

    v4l2_format dst_fmt = {};
    dst_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    dst_fmt.fmt.pix_mp.width = dst_width;
    dst_fmt.fmt.pix_mp.height = dst_height;
    dst_fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    dst_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = dst_stride;
    dst_fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    dst_fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_DEFAULT;
    dst_fmt.fmt.pix_mp.num_planes = 1;
    if (ioctl(fd_, VIDIOC_S_FMT, &dst_fmt) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to set capture format";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    int r =
        src_buffers_.Allocate(fd_, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                              src_memory, NUM_OUTPUT_BUFFERS, &src_fmt, false);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to request output buffers";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    r = dst_buffers_.Allocate(fd_, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                              V4L2_MEMORY_MMAP, NUM_CAPTURE_BUFFERS, &dst_fmt,
                              dst_export_dmafds);
    if (r != WEBRTC_VIDEO_CODEC_OK) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to request output buffers";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    for (int i = 0; i < dst_buffers_.count(); i++) {
      v4l2_plane planes[VIDEO_MAX_PLANES];
      v4l2_buffer v4l2_buf = {};
      v4l2_buf.type = dst_buffers_.type();
      v4l2_buf.memory = dst_buffers_.memory();
      v4l2_buf.index = i;
      v4l2_buf.length = 1;
      v4l2_buf.m.planes = planes;
      if (ioctl(fd_, VIDIOC_QBUF, &v4l2_buf) < 0) {
        RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to queue buffer"
                          << " type=" << dst_buffers_.type()
                          << " memory=" << dst_buffers_.memory()
                          << " index=" << i << " error=" << strerror(errno);
        return WEBRTC_VIDEO_CODEC_ERROR;
      }
    }

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to start output stream";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to start capture stream";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    dst_width_ = dst_width;
    dst_height_ = dst_height;
    dst_stride_ = dst_stride;

    runner_ = V4L2Runner::Create(fd_, src_buffers_.count(), src_memory,
                                 V4L2_MEMORY_MMAP);

    return WEBRTC_VIDEO_CODEC_OK;
  }

 public:
  int Scale(const rtc::scoped_refptr<webrtc::VideoFrameBuffer>& frame_buffer,
            int64_t timestamp_us,
            OnCompleteCallback on_complete) {
    RTC_LOG(LS_INFO) << "V4L2Scaler::Scale";
    std::optional<int> index = runner_->PopAvailableBufferIndex();
    if (!index) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  No available output buffers";
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    rtc::scoped_refptr<webrtc::VideoFrameBuffer> bind_buffer;
    v4l2_buffer v4l2_buf = {};
    v4l2_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    v4l2_buf.index = *index;
    v4l2_buf.field = V4L2_FIELD_NONE;
    v4l2_buf.length = 1;
    v4l2_plane planes[VIDEO_MAX_PLANES] = {};
    v4l2_buf.m.planes = planes;
    v4l2_buf.flags |= V4L2_BUF_FLAG_TIMESTAMP_COPY;
    v4l2_buf.timestamp.tv_sec = timestamp_us / rtc::kNumMicrosecsPerSec;
    v4l2_buf.timestamp.tv_usec = timestamp_us % rtc::kNumMicrosecsPerSec;
    if (frame_buffer->type() == webrtc::VideoFrameBuffer::Type::kNative) {
      v4l2_buf.memory = V4L2_MEMORY_DMABUF;
      auto native_buffer = static_cast<V4L2NativeBuffer*>(frame_buffer.get());
      planes[0].m.fd = native_buffer->fd();
      planes[0].bytesused = native_buffer->size();
      planes[0].length = native_buffer->size();
      bind_buffer = frame_buffer;
    } else {
      v4l2_buf.memory = V4L2_MEMORY_MMAP;

      auto& d_buffer = src_buffers_.at(v4l2_buf.index);

      rtc::scoped_refptr<webrtc::I420BufferInterface> s_buffer =
          frame_buffer->ToI420();
      int width = s_buffer->width();
      int height = s_buffer->height();
      int d_stride = d_buffer.planes[0].bytesperline;
      int d_chroma_stride = (d_stride + 1) / 2;
      int d_chroma_height = (height + 1) / 2;
      uint8_t* d_y = (uint8_t*)d_buffer.planes[0].start;
      uint8_t* d_u = d_y + d_stride * height;
      uint8_t* d_v = d_u + d_chroma_stride * d_chroma_height;
      libyuv::I420Copy(
          s_buffer->DataY(), s_buffer->StrideY(), s_buffer->DataU(),
          s_buffer->StrideU(), s_buffer->DataV(), s_buffer->StrideV(), d_y,
          d_stride, d_u, d_chroma_stride, d_v, d_chroma_stride, width, height);
      bind_buffer = s_buffer;
    }

    runner_->Enqueue(
        &v4l2_buf, [this, bind_buffer, on_complete](
                       v4l2_buffer* v4l2_buf, std::function<void()> on_next) {
          int64_t timestamp_us =
              v4l2_buf->timestamp.tv_sec * rtc::kNumMicrosecsPerSec +
              v4l2_buf->timestamp.tv_usec;
          if (dst_buffers_.dmafds_exported()) {
            auto& plane = dst_buffers_.at(v4l2_buf->index).planes[0];
            RTC_LOG(LS_INFO)
                << "Scale completed: length=" << v4l2_buf->length
                << " fd=" << plane.fd << " bytesused=" << plane.bytesperline;
            auto frame_buffer = rtc::make_ref_counted<V4L2NativeBuffer>(
                webrtc::VideoType::kI420, dst_width_, dst_height_, dst_width_,
                dst_height_, plane.fd, plane.sizeimage, plane.bytesperline,
                [on_next]() { on_next(); });
            on_complete(frame_buffer, timestamp_us);
          } else {
            auto d_buffer = webrtc::I420Buffer::Create(dst_width_, dst_height_);
            int s_chroma_stride = (dst_stride_ + 1) / 2;
            int s_chroma_height = (dst_height_ + 1) / 2;
            auto& plane = dst_buffers_.at(v4l2_buf->index).planes[0];
            uint8_t* s_y = (uint8_t*)plane.start;
            uint8_t* s_u = s_y + dst_stride_ * dst_height_;
            uint8_t* s_v = s_u + s_chroma_stride * s_chroma_height;
            libyuv::I420Copy(s_y, dst_stride_, s_u, s_chroma_stride, s_v,
                             s_chroma_stride, d_buffer->MutableDataY(),
                             d_buffer->StrideY(), d_buffer->MutableDataU(),
                             d_buffer->StrideU(), d_buffer->MutableDataV(),
                             d_buffer->StrideV(), dst_width_, dst_height_);
            on_complete(d_buffer, timestamp_us);
            on_next();
          }
        });

    return WEBRTC_VIDEO_CODEC_OK;
  }

  ~V4L2Scaler() {
    runner_.reset();

    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to stop output stream";
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMOFF, &type) < 0) {
      RTC_LOG(LS_ERROR) << __FUNCTION__ << "  Failed to stop capture stream";
    }

    src_buffers_.Deallocate();
    dst_buffers_.Deallocate();

    close(fd_);
  }

 private:
  int fd_ = 0;

  int dst_width_ = 0;
  int dst_height_ = 0;
  int dst_stride_ = 0;

  V4L2Buffers src_buffers_;
  V4L2Buffers dst_buffers_;

  std::shared_ptr<V4L2Runner> runner_;
};

//class V4L2Decoder {
// private:
//  int fd_ = 0;
//
//  V4L2Buffers src_buffers_;
//  V4L2Buffers dst_buffers_;
//
//  std::shared_ptr<V4L2Runner> runner_;
//};

#endif
