/*******************************************************************************
* Copyright (c) 2014 EclipseSource and others.
* All rights reserved. This program and the accompanying materials
* are made available under the terms of the Eclipse Public License v1.0
* which accompanies this distribution, and is available at
* http://www.eclipse.org/legal/epl-v10.html
*
* Contributors:
*    EclipseSource - initial API and implementation
******************************************************************************/
#include <jni.h>
#include <libplatform/libplatform.h>
#include <iostream>
#include <v8.h>
#include <string.h>
#include <v8-inspector.h>
#include <cstdlib>
#include "com_mv8_V8.h"
#include "com_mv8_V8Isolate.h"
#include "com_mv8_V8Context.h"
#include "com_mv8_V8Value.h"

using namespace std;
using namespace v8;

v8::Platform *v8Platform;
JavaVM* jvm = NULL;
jclass v8ContextCls = NULL;
jmethodID v8CallJavaMethodID = NULL;

class ShellArrayBufferAllocator : public v8::ArrayBuffer::Allocator
{
  public:
	virtual void *Allocate(size_t length)
	{
		void *data = AllocateUninitialized(length);
		return data == NULL ? data : memset(data, 0, length);
	}
	virtual void *AllocateUninitialized(size_t length) { return malloc(length); }
	virtual void Free(void *data, size_t) { free(data); }
};

ShellArrayBufferAllocator array_buffer_allocator;

class V8IsolateData
{
  public:
	Isolate *isolate;
	StartupData startupData;
	Persistent<ObjectTemplate> * globalObjectTemplate;
};

static void javaCallback(const v8::FunctionCallbackInfo<v8::Value>& args) {
	if (args.Length() < 1) return;

	Isolate * isolate = args.GetIsolate();
	Local<Context> context = isolate->GetCurrentContext();
	HandleScope scope(isolate);

	Local<External> data = Local<External>::Cast(context->GetEmbedderData(1));
	jobject javaInstance = reinterpret_cast<jobject>(data->Value());

	JNIEnv * env;
	int getEnvStat = jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
  	if (getEnvStat == JNI_EDETACHED) {
		if (jvm->AttachCurrentThread((void **)&env, NULL) != 0) {
			std::cout << "Failed to attach" << std::endl;
		}
	}
	else if (getEnvStat == JNI_OK) {
	}

	String::Value unicodeString(args[0]->ToString(isolate));
	jstring javaString = env->NewString(*unicodeString, unicodeString.length());
	jobject result = env->CallObjectMethod(javaInstance, v8CallJavaMethodID, javaString);
	
	const uint16_t* resultString = env->GetStringChars((jstring)result, NULL);
	int length = env->GetStringLength((jstring)result);
	Local<String> str = String::NewFromTwoByte(isolate, resultString, String::NewStringType::kNormalString, length);
	env->ReleaseStringChars((jstring)result, resultString);
	args.GetReturnValue().Set(str);
}

JNIEXPORT jlong JNICALL Java_com_mv8_V8__1createIsolate(JNIEnv *env, jclass V8, jstring snapshotBlob)
{
	const char * nativeString;
	V8IsolateData *isolateData = new V8IsolateData();
	v8::Isolate::CreateParams create_params;

	if (snapshotBlob)
	{
		jstring snapshotBlobGlobal = (jstring)env->NewGlobalRef(snapshotBlob);
		nativeString = env->GetStringUTFChars(snapshotBlobGlobal, NULL); // Note: GetStringUTF8Chars does not support emoji's
		isolateData->startupData = v8::V8::CreateSnapshotDataBlob(nativeString);
		create_params.snapshot_blob = &isolateData->startupData;
	}

	create_params.array_buffer_allocator = &array_buffer_allocator;
	isolateData->isolate = v8::Isolate::New(create_params);
	Isolate * isolate = isolateData->isolate;
	v8::Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);

	Handle<ObjectTemplate> globalObject = ObjectTemplate::New(isolate);
	globalObject->Set(String::NewFromUtf8(isolate, "__calljava", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, javaCallback));
	isolateData->globalObjectTemplate = new Persistent<ObjectTemplate>(isolate, globalObject);

	return reinterpret_cast<jlong>(isolateData);
}

JNIEXPORT jlong JNICALL Java_com_mv8_V8Isolate__1createContext(JNIEnv * env, jclass, jlong isolatePtr, jobject javaInstance)
{
	V8IsolateData *isolateData = reinterpret_cast<V8IsolateData *>(isolatePtr);
	Isolate *isolate = isolateData->isolate;
	v8::Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);

	Handle<Context> context = Context::New(isolate, NULL, isolateData->globalObjectTemplate->Get(isolate));
	jobject instanceRef = env->NewGlobalRef(javaInstance);
	Local<External> ext = External::New(isolate, instanceRef);
	context->SetEmbedderData(1, ext);
	Persistent<Context> *persistentContext = new Persistent<Context>(isolate, context);
	return reinterpret_cast<jlong>(persistentContext);
}

JNIEXPORT jlong JNICALL Java_com_mv8_V8Context__1runScript(JNIEnv *env, jclass clz, jlong isolatePtr, jlong contextPtr, jstring scriptSource)
{
	V8IsolateData *isolateData = reinterpret_cast<V8IsolateData *>(isolatePtr);
	Isolate *isolate = isolateData->isolate;
	Persistent<Context> *persistentContext = reinterpret_cast<Persistent<Context> *>(contextPtr);
	Isolate::Scope isolate_scope(isolate);

  	TryCatch try_catch(isolate);
	HandleScope handle_scope(isolate);
	Local<Context> context = persistentContext->Get(isolate);

	const uint16_t* unicodeString = env->GetStringChars(scriptSource, NULL);
	int length = env->GetStringLength(scriptSource);
	Local<String> source = String::NewFromTwoByte(isolate, unicodeString, String::NewStringType::kNormalString, length);
	env->ReleaseStringChars(scriptSource, unicodeString);

	Context::Scope context_scope(context);
	Local<Script> script = Script::Compile(context, source).ToLocalChecked();

	Local<Value> result;
	if (!script->Run(context).ToLocal(&result)) {
		String::Utf8Value error(try_catch.Exception());
	}
	Persistent<Value> *persistentValue = new Persistent<Value>(isolate, result);
	return reinterpret_cast<jlong>(persistentValue);
}

JNIEXPORT void JNICALL Java_com_mv8_V8Context__1dispose (JNIEnv * env, jclass, jlong isolatePtr, jlong contextPtr)
{
	V8IsolateData *isolateData = reinterpret_cast<V8IsolateData *>(isolatePtr);
	Isolate *isolate = isolateData->isolate;
	Persistent<Context> *persistentContext = reinterpret_cast<Persistent<Context> *>(contextPtr);
	HandleScope handle_scope(isolate);

	Local<Context> context = persistentContext->Get(isolate);
	Local<External> data = Local<External>::Cast(context->GetEmbedderData(1));
	jobject javaInstance = reinterpret_cast<jobject>(data->Value());
	env->DeleteGlobalRef(javaInstance);
	persistentContext->Reset();
}

JNIEXPORT jstring JNICALL Java_com_mv8_V8Value__1getStringValue(JNIEnv *env, jclass, jlong isolatePtr, jlong, jlong valuePtr)
{
	V8IsolateData *isolateData = reinterpret_cast<V8IsolateData *>(isolatePtr);
	Isolate *isolate = isolateData->isolate;
	Persistent<Value> *persistentContext = reinterpret_cast<Persistent<Value> *>(valuePtr);
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);
	Local<Value> v = persistentContext->Get(isolate);
	if (v->IsString()) {
		String::Value unicodeString(v->ToString(isolate));
		return env->NewString(*unicodeString, unicodeString.length());
	} else {
		return env->NewStringUTF("Not set");
	}
}

JNIEXPORT jlong JNICALL Java_com_mv8_V8Isolate__1createObjectTemplate(JNIEnv *, jclass, jlong isolatePtr)
{
	V8IsolateData *isolateData = reinterpret_cast<V8IsolateData *>(isolatePtr);
	Isolate *isolate = isolateData->isolate;
	Isolate::Scope isolate_scope(isolate);
	HandleScope handle_scope(isolate);

	Handle<ObjectTemplate> objectTemplate = ObjectTemplate::New(isolate);
	Persistent<ObjectTemplate> *persistent = new Persistent<ObjectTemplate>(isolate, objectTemplate);
	return reinterpret_cast<jlong>(persistent);
}

class MethodDescriptor {
public:
  jlong methodID;
  jlong v8RuntimePtr;
};

void getJNIEnv(JNIEnv*& env) {
  int getEnvStat = jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
  if (getEnvStat == JNI_EDETACHED) {
    if (jvm->AttachCurrentThread((void **)&env, NULL) != 0) {
      std::cout << "Failed to attach" << std::endl;
    }
  }
  else if (getEnvStat == JNI_OK) {
  }
  else if (getEnvStat == JNI_EVERSION) {
    std::cout << "GetEnv: version not supported" << std::endl;
  }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
	JNIEnv *env;
	jint onLoad_err = -1;
	if (vm->GetEnv((void **)&env, JNI_VERSION_1_6) != JNI_OK)
	{
		return onLoad_err;
	}
	if (env == NULL)
	{
		return onLoad_err;
	}

	v8::V8::InitializeICU();
	V8::InitializeExternalStartupData(".");

	v8Platform = v8::platform::CreateDefaultPlatform();
	v8::V8::InitializePlatform(v8Platform);
	v8::V8::Initialize();

	jvm = vm;
    v8ContextCls = (jclass)env->NewGlobalRef((env)->FindClass("com/mv8/V8Context"));
	v8CallJavaMethodID = env->GetMethodID(v8ContextCls, "__calljava", "(Ljava/lang/String;)Ljava/lang/String;");

	return JNI_VERSION_1_6;
}