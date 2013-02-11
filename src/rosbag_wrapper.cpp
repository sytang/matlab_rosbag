#include "mex.h"

#include <string>
#include <map>

#include <wordexp.h>

#include <boost/scoped_ptr.hpp>

#include <ros/ros.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>

#include "parser.hpp"
#include "matlab_util.hpp"

using namespace std;

//======================== ROS to Matlab conversions ========================//

// Extend mexWrap() to handle converting from vectors of vectors of bytes to
// matlab types
#define __CREATE_MEX_WRAP_BYTES(CPP_TYPE, MATLAB_TYPE)                  \
  template<> mxArray*                                                   \
  mexWrap<CPP_TYPE, vector<vector<uint8_t> > >                          \
  (const vector<vector<uint8_t> > &b) {                                 \
    const vector<uint8_t> &bytes = b.back();                            \
    if (bytes.size() % sizeof(CPP_TYPE) != 0) {                         \
      throw runtime_error("bad size");                                  \
    }                                                                   \
    size_t n_elem = bytes.size() / sizeof(CPP_TYPE);                    \
    mxArray *result = mxCreateNumericMatrix(1, n_elem,                  \
                                            MATLAB_TYPE, mxREAL);       \
    uint8_t *data = static_cast<uint8_t*>(mxGetData(result));           \
    copy(bytes.begin(), bytes.end(), data);                             \
    return result;                                                      \
  }

__CREATE_MEX_WRAP_BYTES(uint8_t, mxUINT8_CLASS);
__CREATE_MEX_WRAP_BYTES(uint16_t, mxUINT16_CLASS);
__CREATE_MEX_WRAP_BYTES(uint32_t, mxUINT32_CLASS);
__CREATE_MEX_WRAP_BYTES(uint64_t, mxUINT64_CLASS);

__CREATE_MEX_WRAP_BYTES(int8_t, mxINT8_CLASS);
__CREATE_MEX_WRAP_BYTES(int16_t, mxINT16_CLASS);
__CREATE_MEX_WRAP_BYTES(int32_t, mxINT32_CLASS);
__CREATE_MEX_WRAP_BYTES(int64_t, mxINT64_CLASS);

__CREATE_MEX_WRAP_BYTES(float, mxSINGLE_CLASS);
__CREATE_MEX_WRAP_BYTES(double, mxDOUBLE_CLASS);

mxArray* bytesToString(const vector<uint8_t> &bytes) {
  boost::scoped_array<char> chars(new char[bytes.size() + 1]);
  copy(bytes.begin(), bytes.end(), chars.get());
  *(chars.get() + bytes.size()) = '\0';
  mxArray *result = mxCreateString(chars.get());
  return result;
}

// Specialization for string.  Return a character array if dealing with a
// single array of bytes, else return a cell cell array of character arrays.
template<> mxArray*
mexWrap<string, vector<vector<uint8_t> > >(const vector<vector<uint8_t> > &b) {
  if (b.size() == 1) {
    return bytesToString(b[0]);
  } else {
    mxArray *cells = mxCreateCellMatrix(1, b.size());
    for (int i = 0; i < b.size(); ++i) {
      mxSetCell(cells, 0, bytesToString(b[i]));
    }
    return cells;
  }
}

// Specialization for ros::Time from bytes.
// Return a struct with integer 'sec', 'nsec', fields and a double 'time' field.
template<> mxArray*
mexWrap<ros::Time, vector<vector<uint8_t> > >(const vector<vector<uint8_t> > &b) {
  const char *fields[] = {"sec", "nsec", "time"};
  mxArray *times =
    mxCreateStructMatrix(1, b.size(), sizeof(fields)/sizeof(fields[0]), fields);

  for (int i = 0; i < b.size(); ++i) {
    if (b[i].size() != 8) {
        throw runtime_error("bad size");
    }
    uint32_t sec = 0, nsec = 0;
    copy(b[i].begin(), b[i].begin() + 4, reinterpret_cast<uint8_t*>(&sec));
    copy(b[i].begin() + 4, b[i].end(), reinterpret_cast<uint8_t*>(&nsec));
    mxSetField(times, i, "sec", mexWrap<uint32_t>(sec));
    mxSetField(times, i, "nsec", mexWrap<uint32_t>(nsec));
    mxSetField(times, i, "time", mexWrap<double>(sec + 1e-9 * nsec));
  }
  return times;
}

// Specialization for ros::Time.
// Return a struct with integer 'sec', 'nsec', fields and a doulbe 'time' field.
template<>
mxArray* mexWrap<ros::Time>(const ros::Time &t) {
  const char *fields[] = {"sec", "nsec", "time"};
  mxArray *time =
    mxCreateStructMatrix(1, 1, sizeof(fields) / sizeof(fields[0]), fields);
  mxSetField(time, 0, "sec", mexWrap<uint32_t>(t.sec));
  mxSetField(time, 0, "nsec", mexWrap<uint32_t>(t.nsec));
  mxSetField(time, 0, "time", mexWrap<double>(t.sec + 1e-9 * t.nsec));
  return time;
}

// Specialization for bools.  Return a logical array.
template<> mxArray*
mexWrap<bool, vector<vector<uint8_t> > >(const vector<vector<uint8_t> > &b) {
  mxArray *matrix = mxCreateLogicalMatrix(1, b.size());
  bool *bools = static_cast<bool*>(mxGetData(matrix));
  for (int i = 0; i < b.size(); ++i) {
    if (b[i].size() != 1) {
        throw runtime_error("bad size");
    } else {
      bools[i] = b[i][0];
    }
  }
  return matrix;
}


// Forward declare template specialization; mexWrap<ROSMessage> and
// mexWrap<ROSMessage::Field> call each other, so this breaks the cycle
template<>
mxArray* mexWrap<ROSMessage>(const ROSMessage &m);

template<>
mxArray* mexWrap<ROSMessage::Field>(const ROSMessage::Field &field) {
  // TODO: Should be able to create 1x0 struct with appropriate fields for
  // arrays of empty messages.  Currently, this just checks if field has no
  // entries in which case it returns empty, creating an empty array.
  if (field.size() == 0) {
    return NULL;
  } else if (field.at(0).type().is_builtin) {
    if (field.size() != 1) {
      throw runtime_error("Shouldn't have multiple arrays of builtins");
    }
    return mexWrap<ROSMessage>(field.at(0));
  } else {
    const ROSMessage &msg = field.at(0);
    boost::scoped_array<const char*> names(new const char*[msg.nfields()]);
    for (int i = 0; i < msg.nfields(); ++i) {
      names[i] = msg.at(i).name().c_str();
    }
    mxArray *rv = mxCreateStructMatrix(1, field.size(),
                                       msg.nfields(), names.get());
    for (int i = 0; i < field.size(); ++i) {
      const ROSMessage &msg = field.at(i);
      for (int j = 0; j < msg.nfields(); ++j) {
        mxArray *val = mexWrap<ROSMessage::Field>(msg.at(j));
        mxSetField(rv, i, msg.at(j).name().c_str(), val);
      }
    }
    return rv;
  }
}

// Convert a ROSMessage to matlab.  If the ROSType is a builtin, return the
// equivalent matlab datatype.  Otherwise, return a matlab struct.
template<>
mxArray* mexWrap<ROSMessage>(const ROSMessage &msg) {
  if (!msg.type().is_builtin) {
    // Get fields for this message type
    boost::scoped_array<const char*> names(new const char*[msg.nfields()]);
    for (int i = 0; i < msg.nfields(); ++i) {
      names[i] = msg.at(i).name().c_str();
    }

    // Create an array of structs and then populate it by iterating over each
    // field of each message
    mxArray *rv = mxCreateStructMatrix(1, 1, msg.nfields(), names.get());
    for (int f = 0; f < msg.nfields(); ++f) {
      const ROSMessage::Field &field = msg.at(f);
      mxArray *val = mexWrap<ROSMessage::Field>(field);
      mxSetField(rv, 0, field.name().c_str(), val);
    }
    return rv;
  } else {
    const string &type = msg.type().base_type;
    const vector<vector<uint8_t> > &bytes = msg.bytes();
    if (type == "bool")          { return mexWrap<bool>(bytes); }
    else if (type == "byte")     { return mexWrap<int8_t>(bytes); }
    else if (type == "char")     { return mexWrap<uint8_t>(bytes); }
    else if (type == "uint8")    { return mexWrap<uint8_t>(bytes); }
    else if (type == "uint16")   { return mexWrap<uint16_t>(bytes); }
    else if (type == "uint32")   { return mexWrap<uint32_t>(bytes); }
    else if (type == "uint64")   { return mexWrap<uint64_t>(bytes); }
    else if (type == "int8")     { return mexWrap<int8_t>(bytes); }
    else if (type == "int16")    { return mexWrap<int16_t>(bytes); }
    else if (type == "int32")    { return mexWrap<int32_t>(bytes); }
    else if (type == "int64")    { return mexWrap<int64_t>(bytes); }
    else if (type == "float32")  { return mexWrap<float>(bytes); }
    else if (type == "float64")  { return mexWrap<double>(bytes); }
    else if (type == "time")     { return mexWrap<ros::Time>(bytes); }
    else if (type == "duration") { return mexWrap<ros::Time>(bytes); }
    else if (type == "string")   { return mexWrap<string>(bytes); }
    else { throw invalid_argument("Not a fundamental type"); }
  }
}

//============================= Mex Interfaces ==============================//

class ROSBagWrapper {
public:
  ROSBagWrapper(const string &fname) : path_(fname), view_(NULL) {
    // Word expansion of filename (e.g. tilde expands to home directory)
    wordexp_t fname_exp;
    if (wordexp(fname.c_str(), &fname_exp, 0) != 0) {
      throw invalid_argument("Invalid filename: " + fname);
    }
    string path = fname_exp.we_wordv[0];
    wordfree(&fname_exp);

    bag_.open(path.c_str(), rosbag::bagmode::Read);
  }

  void mex(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    string cmd = mexUnwrap<string>(prhs[0]);
    if (cmd == "resetView") {
      vector<string> topics = mexUnwrap<vector<string> >(prhs[1]);
      resetView(topics);
    } else if (cmd == "readMessage") {
      if (nrhs != 2) {
        error("ROSBagWrapper::mex() Expected two arguments");
      }
      bool meta = mexUnwrap<bool>(prhs[1]);
      if (!meta) {
        readMessage(&plhs[0]);
      } else {
        readMessage(&plhs[0], &plhs[1]);
      }
    } else if (cmd == "readAllMessages") {
      if (nrhs != 2) {
        error("ROSBagWrapper::mex() Expected two arguments");
      }
      bool meta = mexUnwrap<bool>(prhs[1]);
      if (!meta) {
        readAllMessages(&plhs[0]);
      } else {
        readAllMessages(&plhs[0], &plhs[1]);
      }
    } else if (cmd == "hasNext") {
      plhs[0] = mexWrap<bool>(hasNext());
    } else {
      throw invalid_argument("ROSBagWrapper::mex() Unknown method");
    }
  }

  void resetView(const vector<string> &topics) {
    view_.reset(new rosbag::View(bag_, rosbag::TopicQuery(topics)));
    iter_ = view_->begin();
  }

  void readMessage(mxArray **msg) {
    const rosbag::MessageInstance &mi = *iter_;
    boost::scoped_ptr<ROSMessage> rmsg(deser_.CreateMessage(mi));
    *msg = mexWrap<ROSMessage>(*rmsg);
    ++iter_;
  }

  void readMessage(mxArray **msg, mxArray **meta) {
    const rosbag::MessageInstance &mi = *iter_;
    const char *fields[] = {"topic", "time", "datatype"};
    mxArray *val =
      mxCreateStructMatrix(1, 1, sizeof(fields) / sizeof(fields[0]), fields);
    mxSetField(val, 0, "topic", mexWrap<string>(mi.getTopic()));
    mxSetField(val, 0, "time", mexWrap<ros::Time>(mi.getTime()));
    mxSetField(val, 0, "datatype", mexWrap<string>(mi.getDataType()));
    *meta = val;

    readMessage(msg);
  }

  void readAllMessages(mxArray **msg) {
    vector<mxArray*> msgs;
    while (hasNext()) {
      msgs.push_back(NULL);
      readMessage(&msgs.back());
    }
    *msg = mxCreateCellMatrix(1, msgs.size());
    for (int i = 0; i < msgs.size(); ++i) {
      mxSetCell(*msg, i, msgs[i]);
    }
  }

  void readAllMessages(mxArray **msg, mxArray **meta) {
    vector<mxArray*> msgs, metas;
    while (hasNext()) {
      msgs.push_back(NULL);
      metas.push_back(NULL);
      readMessage(&msgs.back(), &metas.back());
    }
    *msg = mxCreateCellMatrix(1, msgs.size());
    *meta = mxCreateCellMatrix(1, metas.size());
    for (int i = 0; i < msgs.size(); ++i) {
      mxSetCell(*msg, i, msgs[i]);
      mxSetCell(*meta, i, metas[i]);
    }
  }

  bool hasNext() const {
    return view_ != NULL && iter_ != view_->end();
  }

private:
  string path_;
  rosbag::Bag bag_;
  boost::scoped_ptr<rosbag::View> view_;
  rosbag::View::iterator iter_;
  BagDeserializer deser_;
};

// Manage separate instances of bags
class InstanceManager {
public:
  InstanceManager() : id_ctr_(1) {}

  ~InstanceManager() {
    for (map<uint64_t, ROSBagWrapper*>::iterator it = handles_.begin();
         it != handles_.end();
         ++it) {
      delete (*it).second;
    }
  }

  void mex(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    string cmd = mexUnwrap<string>(prhs[0]);
    if (cmd == "construct") {
      string bagname = mexUnwrap<string>(prhs[1]);
      handles_[id_ctr_] = new ROSBagWrapper(bagname);
      plhs[0] = mexWrap<uint64_t>(id_ctr_);
      id_ctr_++;
      id_ctr_ = id_ctr_ == 0 ? id_ctr_ + 1 : id_ctr_;
    } else if (cmd == "destruct") {
      uint64_t id = mexUnwrap<uint64_t>(prhs[1]);
      bool check_exists = nrhs == 3 ? mexUnwrap<bool>(prhs[2]) : true;
      if (check_exists) {
        get(id);
      }
      delete handles_[id];
      handles_.erase(id);
    } else {
      throw invalid_argument("InstanceManger::mex() Unknown method");
    }
  }

  ROSBagWrapper* get(uint64_t id) {
    if (handles_.count(id) == 0) {
      throw invalid_argument("InstanceManager::get() Invalid handle");
    } else {
      return handles_[id];
    }
  }

private:
  uint64_t id_ctr_;                  /**< Next valid ID */
  map<uint64_t, ROSBagWrapper*> handles_;
};

static InstanceManager manager;

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
  if (nrhs < 1) {
    mexErrMsgTxt("rosbag_wrapper id ...");
  }

  uint64_t id = mexUnwrap<uint64_t>(prhs[0]);
  if (id == 0) {
    manager.mex(nlhs, plhs, nrhs - 1, prhs + 1);
  } else {
    ROSBagWrapper *wrapper = manager.get(id);
    wrapper->mex(nlhs, plhs, nrhs - 1, prhs + 1);
  }
}
