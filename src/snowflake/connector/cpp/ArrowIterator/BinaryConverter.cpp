//
// Copyright (c) 2012-2023 Snowflake Computing Inc. All rights reserved.
//

#include "BinaryConverter.hpp"
#include <memory>

namespace sf
{
Logger* BinaryConverter::logger = new Logger("snowflake.connector.BinaryConverter");

BinaryConverter::BinaryConverter(std::shared_ptr<ArrowArrayView> array)
: m_array(array)
{
}

PyObject* BinaryConverter::toPyObject(int64_t rowIndex) const
{
  if(ArrowArrayViewIsNull(m_array.get(), rowIndex)) {
    Py_RETURN_NONE;
  }
  ArrowStringView stringView = ArrowArrayViewGetStringUnsafe(m_array.get(), rowIndex);
    return PyByteArray_FromStringAndSize(stringView.data, stringView.size_bytes);
}

}  // namespace sf
