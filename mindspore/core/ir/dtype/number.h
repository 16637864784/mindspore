/**
 * Copyright 2019 Huawei Technologies Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef MINDSPORE_CORE_IR_DTYPE_NUMBER_H_
#define MINDSPORE_CORE_IR_DTYPE_NUMBER_H_

#include <cstddef>
#include <iostream>
#include <initializer_list>
#include <map>
#include <memory>
#include <utility>
#include <sstream>
#include <string>
#include <vector>
#include <type_traits>
#include <unordered_map>
#include <algorithm>
#include "base/base.h"
#include "ir/named.h"
#include "ir/dtype/type.h"

namespace mindspore {
// Number, abstract class.
class Number : public Object {
 public:
  Number() : Object(kObjectTypeNumber), number_type_(kObjectTypeNumber), nbits_(0) {}
  Number(const TypeId number_type, const int nbits, bool is_generic = true)
      : Object(kObjectTypeNumber, is_generic), number_type_(number_type), nbits_(nbits) {}
  ~Number() override = default;
  MS_DECLARE_PARENT(Number, Object)

  int nbits() const { return nbits_; }

  TypeId number_type() const override { return number_type_; }
  TypeId type_id() const override { return number_type_; }
  TypeId generic_type_id() const override { return kObjectTypeNumber; }

  bool operator==(const Type &other) const override;
  TypePtr DeepCopy() const override { return std::make_shared<Number>(); }
  std::string ToString() const override { return "Number"; }
  std::string ToReprString() const override { return "number"; }
  std::string DumpText() const override { return "Number"; }
  std::string GetTypeName(const std::string &type_name) const {
    std::ostringstream oss;
    oss << type_name;
    if (nbits() != 0) {
      oss << nbits();
    }
    return oss.str();
  }

 private:
  const TypeId number_type_;
  const int nbits_;
};

using NumberPtr = std::shared_ptr<Number>;

// Bool
class Bool : public Number {
 public:
  Bool() : Number(kNumberTypeBool, 8) {}
  ~Bool() override = default;
  MS_DECLARE_PARENT(Bool, Number)

  TypeId generic_type_id() const override { return kNumberTypeBool; }
  TypePtr DeepCopy() const override { return std::make_shared<Bool>(); }
  std::string ToString() const override { return "Bool"; }
  std::string ToReprString() const override { return "bool"; }
  std::string DumpText() const override { return "Bool"; }
};

// Int
class Int : public Number {
 public:
  Int() : Number(kNumberTypeInt, 0) {}
  explicit Int(const int nbits);
  ~Int() override = default;
  MS_DECLARE_PARENT(Int, Number)
  TypeId generic_type_id() const override { return kNumberTypeInt; }
  TypePtr DeepCopy() const override {
    if (nbits() == 0) {
      return std::make_shared<Int>();
    }
    return std::make_shared<Int>(nbits());
  }
  std::string ToString() const override { return GetTypeName("Int"); }
  std::string ToReprString() const override { return nbits() == 0 ? "int_" : GetTypeName("int"); }
  std::string DumpText() const override {
    return nbits() == 0 ? std::string("Int") : std::string("I") + std::to_string(nbits());
  }
};

// UInt
class UInt : public Number {
 public:
  UInt() : Number(kNumberTypeUInt, 0) {}
  explicit UInt(const int nbits);
  TypeId generic_type_id() const override { return kNumberTypeUInt; }

  ~UInt() override {}
  MS_DECLARE_PARENT(UInt, Number)

  TypePtr DeepCopy() const override {
    if (nbits() == 0) {
      return std::make_shared<UInt>();
    }
    return std::make_shared<UInt>(nbits());
  }
  std::string ToString() const override { return GetTypeName("UInt"); }
  std::string ToReprString() const override { return GetTypeName("uint"); }
  std::string DumpText() const override {
    return nbits() == 0 ? std::string("UInt") : std::string("U") + std::to_string(nbits());
  }
};

// Float
class Float : public Number {
 public:
  Float() : Number(kNumberTypeFloat, 0) {}
  explicit Float(const int nbits);
  ~Float() override {}
  MS_DECLARE_PARENT(Float, Number)

  TypeId generic_type_id() const override { return kNumberTypeFloat; }
  TypePtr DeepCopy() const override {
    if (nbits() == 0) {
      return std::make_shared<Float>();
    }
    return std::make_shared<Float>(nbits());
  }
  std::string ToString() const override { return GetTypeName("Float"); }
  std::string ToReprString() const override { return nbits() == 0 ? "float_" : GetTypeName("float"); }
  std::string DumpText() const override {
    return nbits() == 0 ? std::string("Float") : std::string("F") + std::to_string(nbits());
  }
};

// Complex64
class Complex64 : public Number {
 public:
  Complex64() : Number(kNumberTypeComplex64, 64, false) {}
  ~Complex64() override {}
  MS_DECLARE_PARENT(Complex64, Number)

  TypeId generic_type_id() const override { return kNumberTypeComplex64; }
  TypePtr DeepCopy() const override { return std::make_shared<Complex64>(); }
  std::string ToString() const override { return GetTypeName("Complex64"); }
  std::string ToReprString() const override { return nbits() == 0 ? "complex64_" : GetTypeName("complex64"); }
  std::string DumpText() const override {
    return nbits() == 0 ? std::string("Complex64") : std::string("C") + std::to_string(nbits());
  }
};

inline const TypePtr kBool = std::make_shared<Bool>();
inline const TypePtr kInt8 = std::make_shared<Int>(8);
inline const TypePtr kInt16 = std::make_shared<Int>(16);
inline const TypePtr kInt32 = std::make_shared<Int>(32);
inline const TypePtr kInt64 = std::make_shared<Int>(64);
inline const TypePtr kUInt8 = std::make_shared<UInt>(8);
inline const TypePtr kUInt16 = std::make_shared<UInt>(16);
inline const TypePtr kUInt32 = std::make_shared<UInt>(32);
inline const TypePtr kUInt64 = std::make_shared<UInt>(64);
inline const TypePtr kFloat16 = std::make_shared<Float>(16);
inline const TypePtr kFloat32 = std::make_shared<Float>(32);
inline const TypePtr kFloat64 = std::make_shared<Float>(64);
inline const TypePtr kInt = std::make_shared<Int>();
inline const TypePtr kUInt = std::make_shared<UInt>();
inline const TypePtr kFloat = std::make_shared<Float>();
inline const TypePtr kNumber = std::make_shared<Number>();
inline const TypePtr kComplex64 = std::make_shared<Complex64>();
}  // namespace mindspore

#endif  // MINDSPORE_CORE_IR_DTYPE_NUMBER_H_
