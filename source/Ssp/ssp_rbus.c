/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2015 RDK Management
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <rbus/rbus.h>
#include <libgen.h>
#include "ssp_global.h"
#include "safec_lib_common.h"

typedef enum _rbus_legacy_support
{
    RBUS_LEGACY_STRING = 0,    /**< Null terminated string                                           */
    RBUS_LEGACY_INT,           /**< Integer (2147483647 or -2147483648) as String                    */
    RBUS_LEGACY_UNSIGNEDINT,   /**< Unsigned Integer (ex: 4,294,967,295) as String                   */
    RBUS_LEGACY_BOOLEAN,       /**< Boolean as String (ex:"true", "false"                            */
    RBUS_LEGACY_DATETIME,      /**< ISO-8601 format (YYYY-MM-DDTHH:MM:SSZ) as String                 */
    RBUS_LEGACY_BASE64,        /**< Base64 representation of data as String                          */
    RBUS_LEGACY_LONG,          /**< Long (ex: 9223372036854775807 or -9223372036854775808) as String */
    RBUS_LEGACY_UNSIGNEDLONG,  /**< Unsigned Long (ex: 18446744073709551615) as String               */
    RBUS_LEGACY_FLOAT,         /**< Float (ex: 1.2E-38 or 3.4E+38) as String                         */
    RBUS_LEGACY_DOUBLE,        /**< Double (ex: 2.3E-308 or 1.7E+308) as String                      */
    RBUS_LEGACY_BYTE,
    RBUS_LEGACY_NONE
} rbusLegacyDataType_t;

static bool rbusValue_SetParamVal(rbusValue_t value, ULONG contentType, char const* pBuff)
{

    rbusValueType_t type;
    bool rc = false;

    if(RBUS_LEGACY_BYTE == contentType)
    {
        rbusValue_SetBytes(value, (uint8_t*)pBuff, strlen(pBuff));
        rc = true;
    }
    else if( RBUS_LEGACY_BASE64 == contentType)
    {
        CcspTraceWarning(("RBUS_LEGACY_BASE64_TYPE: Base64 type was never used in CCSP so far.\
                    So, Rbus did not support it till now. Since this is the first Base64 query,\
                    please report to get it fixed."));
        rbusValue_SetString(value, pBuff);
        rc = true;
    }
    else
    {
        switch(contentType)
        {
            case RBUS_LEGACY_STRING:        type = RBUS_STRING; break;
            case RBUS_LEGACY_INT:           type = RBUS_INT32; break;
            case RBUS_LEGACY_UNSIGNEDINT:   type = RBUS_UINT32; break;
            case RBUS_LEGACY_BOOLEAN:       type = RBUS_BOOLEAN; break;
            case RBUS_LEGACY_LONG:          type = RBUS_INT64; break;
            case RBUS_LEGACY_UNSIGNEDLONG:  type = RBUS_UINT64; break;
            case RBUS_LEGACY_FLOAT:         type = RBUS_SINGLE; break;
            case RBUS_LEGACY_DOUBLE:        type = RBUS_DOUBLE; break;
            case RBUS_LEGACY_DATETIME:      type = RBUS_DATETIME; break;
            default:                        return rc;
        }
        rc = rbusValue_SetFromString(value, type, pBuff);
    }
    return rc;
}

static void setOutparams(rbusObject_t outParams,char *parameterName, bool val)
{
    rbusValue_t value;

    rbusValue_Init(&value);
    rbusValue_SetBoolean(value,val);
    rbusObject_SetValue(outParams, parameterName, value);
    rbusValue_Release(value);
}

void rbus_type_to_ccsp_type (rbusValueType_t typeVal, enum dataType_e *pType)
{
    switch(typeVal)
    {
        case RBUS_INT16:
        case RBUS_INT32:
            *pType = ccsp_int;
            break;
        case RBUS_UINT16:
        case RBUS_UINT32:
            *pType = ccsp_unsignedInt;
            break;
        case RBUS_INT64:
            *pType = ccsp_long;
            break;
        case RBUS_UINT64:
            *pType = ccsp_unsignedLong;
            break;
        case RBUS_SINGLE:
            *pType = ccsp_float;
            break;
        case RBUS_DOUBLE:
            *pType = ccsp_double;
            break;
        case RBUS_DATETIME:
            *pType = ccsp_dateTime;
            break;
        case RBUS_BOOLEAN:
            *pType = ccsp_boolean;
            break;
        case RBUS_CHAR:
        case RBUS_INT8:
            *pType = ccsp_int;
            break;
        case RBUS_UINT8:
        case RBUS_BYTE:
            *pType = ccsp_byte;
            break;
        case RBUS_STRING:
            *pType = ccsp_string;
            break;
        case RBUS_BYTES:
            *pType = ccsp_base64;
            break;
        case RBUS_PROPERTY:
        case RBUS_OBJECT:
        case RBUS_NONE:
        default:
            *pType = ccsp_none;
            break;
    }
    return;
}

static int setParameterValues_rbus(rbusObject_t inParams, rbusObject_t outParams)
{
    int ret = RBUS_ERROR_SUCCESS, returnStatus = -1;
    rbusValue_t value;
    rbusProperty_t prop;
    rbusValueType_t type;
    int param_size = 0;
    char *parameterName, *parameterValue = NULL;

    prop = rbusObject_GetProperties(inParams);
    while(prop)
    {
        param_size++;
        prop = rbusProperty_GetNext(prop);
    }
    prop = rbusObject_GetProperties(inParams);
    parameterName = (char *)rbusProperty_GetName(prop);
    setOutparams(outParams, parameterName, false);

    value = rbusProperty_GetValue(prop);
    type = rbusValue_GetType(value);
    parameterValue = rbusValue_ToString(value, NULL, 0);
    parameterValStruct_t *val = NULL;
    val = AnscAllocateMemory(sizeof(parameterValStruct_t));
    if(val == NULL)
    {
        CcspTraceInfo(("Memory Allocation failed - %s : %d\n", __FUNCTION__, __LINE__));
        ret = RBUS_ERROR_BUS_ERROR;
    }
    else
    {
        memset(val, 0, sizeof(parameterValStruct_t));
        unsigned int paramNameLen = strlen(parameterName) + 1;
        unsigned int paramValLen = strlen(parameterValue) + 1;

        /* Copy the Name */
        val->parameterName = AnscAllocateMemory(paramNameLen);
        if(val->parameterName == NULL)
        {
            CcspTraceInfo(("Memory Allocation failed - %s : %d\n", __FUNCTION__, __LINE__));
            ret = RBUS_ERROR_BUS_ERROR;
        }
        strcpy_s(val->parameterName, paramNameLen,  parameterName);

       /* Copy the Value */
        val->parameterValue = AnscAllocateMemory(paramValLen);
        memset(val->parameterValue, 0, paramValLen);
        strcpy_s(val->parameterValue, paramValLen,  parameterValue);

        /* Find the matching ccsp_type */
        rbus_type_to_ccsp_type(type, &val->type);

        if (ret == RBUS_ERROR_SUCCESS)
        {
            returnStatus = setParameterValues(0, 0, val, 1, 0, NULL, NULL);
            CcspTraceDebug(("%s Add entry:  param : %s , val : %s %s , return %d \n", __func__,((returnStatus != CCSP_SUCCESS)? "failed" : "success"), val->parameterName, val->parameterValue, returnStatus));
        }
        if (val->parameterName)
        {
                AnscFreeMemory(val->parameterName);
        }
        if (val->parameterValue)
        {
                AnscFreeMemory(val->parameterValue);
        }

        AnscFreeMemory(val);
        free(parameterValue);
    }

    if(CCSP_SUCCESS == returnStatus)
        setOutparams(outParams,parameterName,true);
    return ret;
}

static int getParameterValues_rbus(rbusObject_t inParams, rbusObject_t outParams)
{
    int rc = RBUS_ERROR_SUCCESS;
    char **parameterNames = 0,*str_value;
    int i = 0;
    int size = 0;
    int param_size = 0;
    int result = -1;
    rbusValue_t value;
    rbusProperty_t prop,out_prop;
    rbusValueType_t type;
    parameterValStruct_t **val = 0;

    /* Get inut parameters size*/
    prop = rbusObject_GetProperties(inParams);
    while(prop)
    {
        param_size++;
        prop = rbusProperty_GetNext(prop);
    }
    if (param_size)
    {
        parameterNames  = AnscAllocateMemory(param_size*sizeof(char *));
        memset(parameterNames, 0, param_size*sizeof(char *));
    }
    else
    {
        CcspTraceError(("%s - No input parameters\n",__func__));
        return RBUS_ERROR_BUS_ERROR;
    }
    prop = rbusObject_GetProperties(inParams);
    for(i = 0; i < param_size; i++)
    {
        parameterNames[i] = NULL;
        parameterNames[i] = (char *)rbusProperty_GetName(prop);
        prop = rbusProperty_GetNext(prop);
    }

    result = getParameterValues(0, parameterNames, param_size, &size, &val , NULL);
    if (result)
    {
        for(i = 0; i < size; i++)
        {
            rbusValue_Init(&value);
            if(true == rbusValue_SetParamVal(value, (ULONG)val[i]->type, val[i]->parameterValue))
            {
                type = rbusValue_GetType(value);
                str_value = rbusValue_ToString(value,NULL,0);
                if(str_value)
                {
                    rbusValue_SetFromString(value, type, str_value);
                    rbusProperty_Init(&out_prop,val[i]->parameterName,value);
                    rbusObject_SetProperty(outParams,out_prop);
                    free(str_value);
                    rbusProperty_Release(out_prop);
                    if(NULL == strstr(val[i]->parameterName, "Passphrase"))
                    {
                        CcspTraceInfo(("%s ParammeterName[%d]-%s, ParameterValue:%s\n  ",__func__,i, val[i]->parameterName, val[i]->parameterValue));
                    }
                    else
                    {
                        CcspTraceWarning(("%s Not printing the value of parameter ParameterName[%d]-%s as it disclose the confidential information\n",__func__,i, val[i]->parameterName));
                    }
                }
                else
                {
                    CcspTraceError(("%s Unable to set the value %s of type %lu",__func__,val[i]->parameterValue,(ULONG)val[i]->type));
                }
                rbusValue_Release(value);

                if(val[i]->parameterName)
                {
                    AnscFreeMemory(val[i]->parameterName);
                }
                if(val[i]->parameterValue)
                {
                    AnscFreeMemory(val[i]->parameterValue);
                }
                AnscFreeMemory(val[i]);
             }
         }
         AnscFreeMemory(val);
    }
    else
        rc = RBUS_ERROR_BUS_ERROR;
    AnscFreeMemory(parameterNames);
    return rc;
}

static int getParameterNames_rbus(rbusObject_t inParams, rbusObject_t outParams)
{
    int rc = RBUS_ERROR_SUCCESS;
    char *parameterName = 0;
    int i = 0;
    int size = 0;
    int result = 0;
    bool nextLevel = 0;
    parameterInfoStruct_t **val = 0;
    rbusProperty_t prop, out_prop;
    /* Get parameter name */
    parameterName = NULL;
    prop = rbusObject_GetProperties(inParams);
    parameterName = (char *)rbusProperty_GetName(prop);
    prop = rbusProperty_GetNext(prop);
    if (prop)
    {
        nextLevel = rbusValue_GetBoolean(rbusProperty_GetValue(prop));
    }
    CcspTraceInfo(("%s QUERY: parameterName=%s, nextLevel=%d\n", __func__, parameterName ? parameterName : "NULL", nextLevel));
    result = getParameterNames(parameterName, nextLevel, &size, &val, NULL);
    if (result == CCSP_SUCCESS)
    {
        CcspTraceInfo(("%s RESULT: Found %d parameter names for query parameterName=%s\n", __func__, size, parameterName ? parameterName : "NULL"));
        for(i = 0; i < size; i++)
        {
            rbusValue_t writableValue;
            rbusValue_Init(&writableValue);
            rbusValue_SetInt32(writableValue, val[i]->writable);
            rbusProperty_Init(&out_prop, val[i]->parameterName, writableValue);
            rbusObject_SetProperty(outParams, out_prop);
            rbusProperty_Release(out_prop);
            rbusValue_Release(writableValue);
            CcspTraceInfo(("%s-ParameterName[%d]-%s\n", __func__, i, val[i]->parameterName));
            if(val[i]->parameterName)
                AnscFreeMemory(val[i]->parameterName);
            AnscFreeMemory(val[i]);
        }
        AnscFreeMemory(val);
    }
    else
        rc = RBUS_ERROR_BUS_ERROR;
    return rc;
}

static int delParameterValues_rbus(rbusObject_t inParams, rbusObject_t outParams)
{
    int rc = RBUS_ERROR_SUCCESS, result = -1;
    rbusProperty_t prop;
    int i = 0;
    int param_size = 0;

    prop = rbusObject_GetProperties(inParams);
    while(prop)
    {
       param_size++;
        prop = rbusProperty_GetNext(prop);
    }
    prop = rbusObject_GetProperties(inParams);
    /* Copy Parameter Name */
    for (i = 0; i < param_size; i++)
    {
        parameterAttributeStruct_t *parameterAttribute = 0;
        parameterAttribute = AnscAllocateMemory(1*sizeof(parameterAttributeStruct_t));
        memset(parameterAttribute, 0, 1*sizeof(parameterAttributeStruct_t));
        unsigned int ParameterNameLen = strlen(rbusProperty_GetName(prop))+1;
        parameterAttribute->parameterName = AnscAllocateMemory(ParameterNameLen);
        strcpy_s(parameterAttribute->parameterName, ParameterNameLen, rbusProperty_GetName(prop));

        /*Set Parameter Attributes*/
        parameterAttribute->notificationChanged = 0;
        parameterAttribute->notification = 0;
        parameterAttribute->access = 0;
        parameterAttribute->accessControlChanged = 1;
        parameterAttribute->accessControlBitmask = 0;

        result = setParameterAttributes(0, parameterAttribute, 1, NULL);
        if (result == CCSP_SUCCESS)
        {
            setOutparams(outParams,parameterAttribute->parameterName,true);
            CcspTraceDebug(("%s delete entry: %s success \n", __func__,parameterAttribute->parameterName));
        }
        else
        {
            CcspTraceError(("%s Failed to delete entry: %s \n", __func__,parameterAttribute->parameterName));
            setOutparams(outParams,parameterAttribute->parameterName,false);
        }
        AnscFreeMemory(parameterAttribute);
        prop = rbusProperty_GetNext(prop);
    }
    return rc;
}

static rbusError_t psmDel(rbusHandle_t handle, char const* methodName, rbusObject_t inParams, rbusObject_t outParams, rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle;
    (void)asyncHandle;
    (void)methodName;
    return delParameterValues_rbus(inParams,outParams);
}

static rbusError_t psmGetNames(rbusHandle_t handle, char const* methodName, rbusObject_t inParams, rbusObject_t outParams, rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle;
    (void)asyncHandle;
    (void)methodName;
    return getParameterNames_rbus(inParams, outParams);
}

static rbusError_t psmGet(rbusHandle_t handle, char const* methodName, rbusObject_t inParams, rbusObject_t outParams, rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle;
    (void)asyncHandle;
    (void)methodName;
    return getParameterValues_rbus(inParams, outParams);
}

static rbusError_t psmSet(rbusHandle_t handle, char const* methodName, rbusObject_t inParams, rbusObject_t outParams, rbusMethodAsyncHandle_t asyncHandle)
{
    (void)handle;
    (void)asyncHandle;
    (void)methodName;
    return setParameterValues_rbus(inParams, outParams);
}

int PsmRbusInit()
{
    rbusHandle_t handle;
    int rc = RBUS_ERROR_SUCCESS;
    char *component_name = "rbusPsmSsp";

    rc = rbus_open(&handle, component_name);
    if(rc != RBUS_ERROR_SUCCESS)
    {
        CcspTraceWarning(("%s: rbus_open failed: %d\n", component_name, rc));
        return rc;
    }
#define dataElementsCount sizeof(dataElements)/sizeof(rbusDataElement_t)
    rbusDataElement_t dataElements[] = {
        {"SetPSMRecordValue()", RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, psmSet}},
        {"DeletePSMRecord()",   RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, psmDel}},
        {"GetPSMRecordValue()", RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, psmGet}},
        {"GetPSMRecordName()", RBUS_ELEMENT_TYPE_METHOD, {NULL, NULL, NULL, NULL, NULL, psmGetNames}}
    };

    rc = rbus_regDataElements(handle, dataElementsCount, dataElements);
    if(rc != RBUS_ERROR_SUCCESS)
    {
        CcspTraceWarning(("%s: rbus_regDataElements failed: %d\n", component_name,rc));
        rbus_unregDataElements(handle, dataElementsCount, dataElements);
        rbus_close(handle);
        CcspTraceWarning(("%s: exit\n",component_name));
    }
    return rc;
}
