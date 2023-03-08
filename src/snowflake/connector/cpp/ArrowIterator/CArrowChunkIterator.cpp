//
// Copyright (c) 2012-2023 Snowflake Computing Inc. All rights reserved.
//

#include "CArrowChunkIterator.hpp"
#include "SnowflakeType.hpp"
#include "IntConverter.hpp"
#include "StringConverter.hpp"
#include "FloatConverter.hpp"
#include "DecimalConverter.hpp"
#include "BinaryConverter.hpp"
#include "BooleanConverter.hpp"
#include "DateConverter.hpp"
#include "TimeStampConverter.hpp"
#include "TimeConverter.hpp"
#include "nanoarrow.h"
#include "arrow/c/bridge.h"
#include <memory>
#include <string>
#include <vector>
#include <iostream>

#define SF_CHECK_PYTHON_ERR() \
  if (py::checkPyError())\
  {\
    PyObject *type, * val, *traceback;\
    PyErr_Fetch(&type, &val, &traceback);\
    PyErr_Clear();\
    m_currentPyException.reset(val);\
\
    Py_XDECREF(type);\
    Py_XDECREF(traceback);\
\
    return std::make_shared<ReturnVal>(nullptr, m_currentPyException.get());\
  }


namespace sf
{

CArrowChunkIterator::CArrowChunkIterator(PyObject* context, std::vector<std::shared_ptr<arrow::RecordBatch>> *batches,
                                         PyObject* use_numpy)
: CArrowIterator(batches), m_latestReturnedRow(nullptr), m_context(context)
{
  m_batchCount = m_cRecordBatches->size();
  m_columnCount = m_batchCount > 0 ? (*m_cRecordBatches)[0]->num_columns() : 0;
  m_currentBatchIndex = -1;
  m_rowIndexInBatch = -1;
  m_rowCountInBatch = 0;
  m_latestReturnedRow.reset();
  m_useNumpy = PyObject_IsTrue(use_numpy);

  logger->debug(__FILE__, __func__, __LINE__, "Arrow chunk info: batchCount %d, columnCount %d, use_numpy: %d", m_batchCount,
               m_columnCount, m_useNumpy);
}

std::shared_ptr<ReturnVal> CArrowChunkIterator::next()
{
  m_rowIndexInBatch++;

  if (m_rowIndexInBatch < m_rowCountInBatch)
  {
    this->createRowPyObject();
    SF_CHECK_PYTHON_ERR()
    return std::make_shared<ReturnVal>(m_latestReturnedRow.get(), nullptr);
  }
  else
  {
    m_currentBatchIndex++;
    if (m_currentBatchIndex < m_batchCount)
    {
      m_rowIndexInBatch = 0;
      m_rowCountInBatch = (*m_cRecordBatches)[m_currentBatchIndex]->num_rows();
      this->initColumnConverters();
      SF_CHECK_PYTHON_ERR()

      logger->debug(__FILE__, __func__, __LINE__, "Current batch index: %d, rows in current batch: %d",
                  m_currentBatchIndex, m_rowCountInBatch);

      this->createRowPyObject();
      SF_CHECK_PYTHON_ERR()

      return std::make_shared<ReturnVal>(m_latestReturnedRow.get(), nullptr);
    }
  }

  /** It looks like no one will decrease the ref of this Py_None, so we don't
   * increment the ref count here */
  return std::make_shared<ReturnVal>(Py_None, nullptr);
}

void CArrowChunkIterator::createRowPyObject()
{
  m_latestReturnedRow.reset(PyTuple_New(m_columnCount));
  for (int i = 0; i < m_columnCount; i++)
  {
    // PyTuple_SET_ITEM steals a reference to the PyObject returned by toPyObject below
    PyTuple_SET_ITEM(
        m_latestReturnedRow.get(), i,
        m_currentBatchConverters[i]->toPyObject(m_rowIndexInBatch));
  }
  return;
}

void CArrowChunkIterator::initColumnConverters()
{
  m_currentBatchConverters.clear();
  std::shared_ptr<arrow::RecordBatch> currentBatch =
      (*m_cRecordBatches)[m_currentBatchIndex];
  m_currentSchema = currentBatch->schema();

  ArrowSchema nanoarrowSchema;
  // TODO: Export is not needed when using nanoarrow IPC to read schema
  arrow::ExportSchema(*m_currentSchema, &nanoarrowSchema);

  for (int i = 0; i < currentBatch->num_columns(); i++)
  {
    std::shared_ptr<arrow::Array> columnArray = currentBatch->column(i);
    std::shared_ptr<arrow::DataType> dt = m_currentSchema->field(i)->type();
    std::shared_ptr<const arrow::KeyValueMetadata> metaData =
        m_currentSchema->field(i)->metadata();

    ArrowSchema nanoarrowColumnSchema = *nanoarrowSchema.children[i];
    ArrowSchemaView nanoarrowColumnSchemaView;
    ArrowError error;
    ArrowSchemaViewInit(&nanoarrowColumnSchemaView, &nanoarrowColumnSchema, &error);

    std::shared_ptr<ArrowArray> nanoarrowColumnArrowArray = std::make_shared<ArrowArray>();
    std::shared_ptr<ArrowArrayView> nanoarrowColumnArrowArrayViewInstance = std::make_shared<ArrowArrayView>();
    arrow::ExportArray(*columnArray, nanoarrowColumnArrowArray.get());

    int res = 0;
    res = ArrowArrayViewInitFromSchema(nanoarrowColumnArrowArrayViewInstance.get(), &nanoarrowColumnSchema, &error);
    if(res != NANOARROW_OK) {
        std::string errorInfo = Logger::formatString("ArrowArrayViewInitFromSchema failure");
        logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
        PyErr_SetString(PyExc_Exception, errorInfo.c_str());
    }
    res = ArrowArrayViewSetArray(nanoarrowColumnArrowArrayViewInstance.get(), nanoarrowColumnArrowArray.get(), &error);
    if(res != NANOARROW_OK) {
        std::string errorInfo = Logger::formatString("ArrowArrayViewSetArray failure");
        logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
        PyErr_SetString(PyExc_Exception, errorInfo.c_str());
    }
    ArrowStringView snowflakeLogicalType;
    const char* metadata = nanoarrowSchema.children[i]->metadata;
    ArrowMetadataGetValue(metadata, ArrowCharView("logicalType"), &snowflakeLogicalType);
    SnowflakeType::Type st = SnowflakeType::snowflakeTypeFromString(
        std::string(snowflakeLogicalType.data, snowflakeLogicalType.size_bytes)
    );

    switch (st)
    {
      case SnowflakeType::Type::FIXED:
      {
        ArrowStringView scaleString;
        ArrowStringView precisionString;
        int scale = 0;
        int precision = 38;
        if(metadata != nullptr) {
            ArrowMetadataGetValue(metadata, ArrowCharView("scale"), &scaleString);
            ArrowMetadataGetValue(metadata, ArrowCharView("precision"), &precisionString);
            scale = std::stoi(scaleString.data);
            precision = std::stoi(precisionString.data);
        }

        switch(nanoarrowColumnSchemaView.type)
        {
#define _SF_INIT_FIXED_CONVERTER(ARROW_TYPE) \
          case ArrowType::ARROW_TYPE: \
          {\
            if (scale > 0)\
            {\
              if (m_useNumpy)\
              {\
                m_currentBatchConverters.push_back(std::make_shared<\
                    sf::NumpyDecimalConverter<ArrowArrayView>>(\
                    nanoarrowColumnArrowArrayViewInstance, precision, scale, m_context));\
              }\
              else\
              {\
                m_currentBatchConverters.push_back(std::make_shared<\
                    sf::DecimalFromIntConverter<ArrowArrayView>>(\
                    nanoarrowColumnArrowArrayViewInstance, precision, scale));\
              }\
            }\
            else\
            {\
              if (m_useNumpy)\
              {\
                m_currentBatchConverters.push_back(\
                    std::make_shared<sf::NumpyIntConverter<ArrowArrayView>>(\
                    nanoarrowColumnArrowArrayViewInstance, m_context));\
              }\
              else\
              {\
                m_currentBatchConverters.push_back(\
                    std::make_shared<sf::IntConverter<ArrowArrayView>>(\
                    nanoarrowColumnArrowArrayViewInstance));\
              }\
            }\
            break;\
          }

          _SF_INIT_FIXED_CONVERTER(NANOARROW_TYPE_INT8)
          _SF_INIT_FIXED_CONVERTER(NANOARROW_TYPE_INT16)
          _SF_INIT_FIXED_CONVERTER(NANOARROW_TYPE_INT32)
          _SF_INIT_FIXED_CONVERTER(NANOARROW_TYPE_INT64)
#undef _SF_INIT_FIXED_CONVERTER

          case ArrowType::NANOARROW_TYPE_DECIMAL128:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::DecimalFromDecimalConverter>(nanoarrowColumnArrowArrayViewInstance,
                                                                  scale));
            break;
          }

          default:
          {
            std::string errorInfo = Logger::formatString(
                "[Snowflake Exception] unknown arrow internal data type(%d) "
                "for FIXED data",
                dt->id());
            logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
            PyErr_SetString(PyExc_Exception, errorInfo.c_str());
            return;
          }
        }
        break;
      }

      case SnowflakeType::Type::ANY:
      case SnowflakeType::Type::CHAR:
      case SnowflakeType::Type::OBJECT:
      case SnowflakeType::Type::VARIANT:
      case SnowflakeType::Type::TEXT:
      case SnowflakeType::Type::ARRAY:
      {
        m_currentBatchConverters.push_back(
            std::make_shared<sf::StringConverter>(nanoarrowColumnArrowArrayViewInstance));
        break;
      }

      case SnowflakeType::Type::BOOLEAN:
      {
        m_currentBatchConverters.push_back(
            std::make_shared<sf::BooleanConverter>(nanoarrowColumnArrowArrayViewInstance));
        break;
      }

      case SnowflakeType::Type::REAL:
      {
        if (m_useNumpy)
        {
          m_currentBatchConverters.push_back(
              std::make_shared<sf::NumpyFloat64Converter>(nanoarrowColumnArrowArrayViewInstance, m_context));
        }
        else
        {
          m_currentBatchConverters.push_back(
              std::make_shared<sf::FloatConverter>(nanoarrowColumnArrowArrayViewInstance));
        }
        break;
      }

      case SnowflakeType::Type::DATE:
      {
        if (m_useNumpy)
        {
          m_currentBatchConverters.push_back(
              std::make_shared<sf::NumpyDateConverter>(nanoarrowColumnArrowArrayViewInstance, m_context));
        }
        else
        {
          m_currentBatchConverters.push_back(
              std::make_shared<sf::DateConverter>(nanoarrowColumnArrowArrayViewInstance));
        }
        break;
      }

      case SnowflakeType::Type::BINARY:
      {
        m_currentBatchConverters.push_back(
            std::make_shared<sf::BinaryConverter>(nanoarrowColumnArrowArrayViewInstance));
        break;
      }

      case SnowflakeType::Type::TIME:
      {
        int scale = 9;
        if(metadata != nullptr) {
            ArrowStringView scaleString;
            ArrowMetadataGetValue(metadata, ArrowCharView("scale"), &scaleString);
            scale = std::stoi(scaleString.data);
        }
        switch (nanoarrowColumnSchemaView.type)
        {
          case NANOARROW_TYPE_INT32:
          case NANOARROW_TYPE_INT64:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::TimeConverter<ArrowArrayView>>(
                    nanoarrowColumnArrowArrayViewInstance, scale));
            break;
          }

          default:
          {
            std::string errorInfo = Logger::formatString(
                "[Snowflake Exception] unknown arrow internal data type(%d) "
                "for TIME data",
                dt->id());
            logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
            PyErr_SetString(PyExc_Exception, errorInfo.c_str());
            return;
          }
        }
        break;
      }

      case SnowflakeType::Type::TIMESTAMP_NTZ:
      {
        int scale = 9;
        if(metadata != nullptr) {
            ArrowStringView scaleString;
            ArrowMetadataGetValue(metadata, ArrowCharView("scale"), &scaleString);
            scale = std::stoi(scaleString.data);
        }
        switch (nanoarrowColumnSchemaView.type)
        {
          case NANOARROW_TYPE_INT64:
          {
            if (m_useNumpy)
            {
              m_currentBatchConverters.push_back(
                  std::make_shared<sf::NumpyOneFieldTimeStampNTZConverter>(
                      nanoarrowColumnArrowArrayViewInstance, scale, m_context));
            }
            else
            {
              m_currentBatchConverters.push_back(
                  std::make_shared<sf::OneFieldTimeStampNTZConverter>(
                      nanoarrowColumnArrowArrayViewInstance, scale, m_context));
            }
            break;
          }

          case NANOARROW_TYPE_STRUCT:
          {
            if (m_useNumpy)
            {
              m_currentBatchConverters.push_back(
                  std::make_shared<sf::NumpyTwoFieldTimeStampNTZConverter>(
                      columnArray, scale, m_context));
            }
            else
            {
              m_currentBatchConverters.push_back(
                  std::make_shared<sf::TwoFieldTimeStampNTZConverter>(
                      columnArray, scale, m_context));
            }
            break;
          }

          default:
          {
            std::string errorInfo = Logger::formatString(
                "[Snowflake Exception] unknown arrow internal data type(%d) "
                "for TIMESTAMP_NTZ data",
                dt->id());
            logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
            PyErr_SetString(PyExc_Exception, errorInfo.c_str());
            return;
          }
        }
        break;
      }

      case SnowflakeType::Type::TIMESTAMP_LTZ:
      {
        int scale = metaData
                        ? std::stoi(metaData->value(metaData->FindKey("scale")))
                        : 9;
        switch (dt->id())
        {
          case arrow::Type::type::INT64:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::OneFieldTimeStampLTZConverter>(
                    nanoarrowColumnArrowArrayViewInstance, scale, m_context));
            break;
          }

          case arrow::Type::type::STRUCT:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::TwoFieldTimeStampLTZConverter>(
                    columnArray, scale, m_context));
            break;
          }

          default:
          {
            std::string errorInfo = Logger::formatString(
                "[Snowflake Exception] unknown arrow internal data type(%d) "
                "for TIMESTAMP_LTZ data",
                dt->id());
            logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
            PyErr_SetString(PyExc_Exception, errorInfo.c_str());
            return;
          }
        }
        break;
      }

      case SnowflakeType::Type::TIMESTAMP_TZ:
      {
        int scale = metaData
                        ? std::stoi(metaData->value(metaData->FindKey("scale")))
                        : 9;
        int byteLength =
            metaData
                ? std::stoi(metaData->value(metaData->FindKey("byteLength")))
                : 16;
        switch (byteLength)
        {
          case 8:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::TwoFieldTimeStampTZConverter>(
                    columnArray, scale, m_context));
            break;
          }

          case 16:
          {
            m_currentBatchConverters.push_back(
                std::make_shared<sf::ThreeFieldTimeStampTZConverter>(
                    columnArray, scale, m_context));
            break;
          }

          default:
          {
            std::string errorInfo = Logger::formatString(
                "[Snowflake Exception] unknown arrow internal data type(%d) "
                "for TIMESTAMP_TZ data",
                dt->id());
            logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
            PyErr_SetString(PyExc_Exception, errorInfo.c_str());
            return;
          }
        }

        break;
      }

      default:
      {
        std::string errorInfo = Logger::formatString(
            "[Snowflake Exception] unknown snowflake data type : %s",
            metaData->value(metaData->FindKey("logicalType")).c_str());
        logger->error(__FILE__, __func__, __LINE__, errorInfo.c_str());
        PyErr_SetString(PyExc_Exception, errorInfo.c_str());
        return;
      }
    }
  }
}

DictCArrowChunkIterator::DictCArrowChunkIterator(PyObject* context,
                                                 std::vector<std::shared_ptr<arrow::RecordBatch>> * batches,
                                                 PyObject* use_numpy)
: CArrowChunkIterator(context, batches, use_numpy)
{
}

void DictCArrowChunkIterator::createRowPyObject()
{
  m_latestReturnedRow.reset(PyDict_New());
  for (int i = 0; i < m_currentSchema->num_fields(); i++)
  {
    py::UniqueRef value(m_currentBatchConverters[i]->toPyObject(m_rowIndexInBatch));
    if (!value.empty())
    {
      // PyDict_SetItemString doesn't steal a reference to value.get().
      PyDict_SetItemString(
          m_latestReturnedRow.get(),
          m_currentSchema->field(i)->name().c_str(),
          value.get());
    }
  }
  return;
}

}  // namespace sf
