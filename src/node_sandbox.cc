#include "node.h"
#include "node_sandbox.h"

#include "env.h"
#include "env-inl.h"
#include "v8.h"
#include "uv.h"


#include <unistd.h>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>


namespace node {
	namespace Sandbox {
		using v8::Context;
		using v8::Function;
		using v8::FunctionCallbackInfo;
		using v8::FunctionTemplate;
		using v8::Handle;
		using v8::HandleScope;
		using v8::Isolate;
		using v8::Local;
		using v8::Object;
		using v8::Persistent;
		using v8::String;
		using v8::Value;
		using v8::TryCatch;
		using v8::JSON;
		using v8::Integer;
		using v8::Number;
		using namespace std;

		static unsigned long int unique_id = 1;

		struct Sandbox_req
		{
			const char *data;
			size_t data_length;
			Isolate* isolate;
			unsigned int callback_id;

			Persistent<Function> callback;
		};

		uv_pipe_t stdin_pipe;
		uv_pipe_t stdout_pipe;

		Persistent<Function> pfn;

		struct CBItem
		{
			unsigned int callback_id;
			Persistent<Function> callback;
		};

		vector<Sandbox_req *> cbs;

		void OnMessageResponse(const FunctionCallbackInfo<Value>& args);
		Handle<Object> jsonParse(Isolate* isolate, Handle<String> input);
		uint8_t* jsonStringify(Isolate* isolate, Handle<Object> input);

		void consoleLog(const char* output, ssize_t lenght) {
			write(0, output, lenght);
		}

		void registerMessage(uv_work_t *req) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);
			write(4, data->data, data->data_length);
		}

		void after_registerMessage(uv_work_t *req, int status) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);

			// register callback
			cbs.push_back(data);
		}

		void SendMessage(Environment* env, const char *data, size_t data_length, unsigned int callback_id, Handle<Function> callback) {
			Sandbox_req* request = new Sandbox_req;

			request->data = data;
			request->data_length = data_length;
			request->isolate = env->isolate();
			request->callback.Reset(env->isolate(), callback);
			request->callback_id = callback_id;

			uv_work_t* req = new uv_work_t();
			req->data = request;
			uv_queue_work(env->event_loop(), req, registerMessage, after_registerMessage);
		}

		/* Pipes */

		// Read
		void alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
			*buf = uv_buf_init((char*) malloc(suggested_size), suggested_size);
		}

		void recieveWork(uv_work_t *req) {
		}

		void after_recieveWork(uv_work_t *req, int status) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);
			Local<Function> callback_fn = Local<Function>::New(data->isolate, pfn);

			Handle<String> response_str = String::NewFromUtf8(data->isolate, data->data, String::kNormalString, data->data_length);
			Handle<Object> response = jsonParse(data->isolate, response_str);

			Local<FunctionTemplate> tpl = FunctionTemplate::New(data->isolate, OnMessageResponse);
			Local<Function> callback =  tpl->GetFunction();

			Local<Value> args[] = {
				response->Get(String::NewFromUtf8(data->isolate, "message")),
				callback
			};

			intptr_t callback_id = response->Get(String::NewFromUtf8(data->isolate, "callback_id"))->Uint32Value();
			//data->isolate->SetData(0, (void*) reinterpret_cast<void*>(callback_id));

			v8::TryCatch try_catch;
			callback_fn->Call(data->isolate->GetCurrentContext()->Global(), 2, args);
			if (try_catch.HasCaught()) {
				node::FatalException(try_catch);
			}
		}

		void findCallback(uv_work_t *req) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);
			consoleLog((char*)data->data, data->data_length);

			// find callback
			unsigned int cb_id = data->callback_id;

			Sandbox_req *callData;
			for (auto &i : cbs) {
				if (i->callback_id == data->callback_id) {
					callData = i;
					break;
				}
			}

			data->callback.Reset(data->isolate, callData->callback);
		}

		void after_findCallback(uv_work_t *req, int status) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);
			Handle<String> response_str = String::NewFromUtf8(data->isolate, data->data, String::kNormalString, data->data_length);
			Handle<Object> response = jsonParse(data->isolate, response_str);

			Local<Value> args[] = {
				response->Get(String::NewFromUtf8(data->isolate, "error")),
				response->Get(String::NewFromUtf8(data->isolate, "response"))
			};

			Local<Function> callback_fn = Local<Function>::New(data->isolate, data->callback);

			v8::TryCatch try_catch;
			callback_fn->Call(data->isolate->GetCurrentContext()->Global(), 2, args);
			if (try_catch.HasCaught()) {
				node::FatalException(try_catch);
			}
		}

		void read_stdin(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
			 Isolate *isolate = Isolate::GetCurrent();
             Environment *env = Environment::GetCurrent(isolate->GetCurrentContext());

			if (nread < 0){
				if (nread == UV_EOF){
					uv_close((uv_handle_t *)&stdin_pipe, NULL);
					uv_close((uv_handle_t *)&stdout_pipe, NULL);
				}
			} else if (nread > 0) {
				// get json and type
				string responseStr((char*)buf->base, nread);
				size_t index = 0;
                while (true) {
                     /* Locate the substring to replace. */
                     index = responseStr.find("}{", index);
                     if (index == string::npos) break;

                     /* Make the replacement. */
                     responseStr.replace(index, 2, "}====0===={");

                     /* Advance index forward so the next iteration doesn't pick it up as well. */
                     index += 11;
                }

				std::string sep("====0====");
                std::vector<string> jsonObjects;

				std::string::size_type start = 0;
				std::string::size_type finish = 0;
				do {
					finish = responseStr.find(sep, start);
					string word = responseStr.substr(start, finish-start);
					jsonObjects.push_back(word);
					start = finish + sep.size();
				} while (finish != string::npos);

				for (vector<int>::size_type i = 0; i != jsonObjects.size(); i++) {
						Handle<String> message = String::NewFromUtf8(env->isolate(), jsonObjects[i].c_str(), String::kNormalString, jsonObjects[i].size());
                		Handle<Object> response = jsonParse(env->isolate(), message);
                		Local<Value> typeValue = response->Get(String::NewFromUtf8(env->isolate(), "type"));

                		if (typeValue->IsNull() || typeValue->IsUndefined()) {
                			return ThrowError(env->isolate(), "needs type argument");
                		}

                		Local<String> type = typeValue->ToString();

                		if (type->Equals(String::NewFromUtf8(env->isolate(), "crypti_call"))) {
                			Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

                			if (callback_id->IsNull() || typeValue->IsUndefined()) {
                				return ThrowError(env->isolate(), "needs callback_id argument");
                			}

                			if (!callback_id->IsNumber()) {
                				return ThrowError(env->isolate(), "callback_id argument should be a number");
                			}

                			Local<Value> messageObj = response->Get(String::NewFromUtf8(env->isolate(), "message"));

                			if (messageObj->IsNull() || messageObj->IsUndefined()) {
                				return ThrowError(env->isolate(), "needs message argument");
                			}

                			if (!messageObj->IsObject()) {
                				return ThrowError(env->isolate(), "message argument should be an object");
                			}

							consoleLog("Send this\n", 10);
                			Sandbox_req* request = new Sandbox_req;
                			request->data = (char*)jsonObjects[i].c_str();
                            request->data_length = (size_t)jsonObjects[i].size();
                			request->isolate = env->isolate();
                			request->callback.Reset(env->isolate(), pfn);

                			uv_work_t req;
                			req.data = request;

                			// call or response
                			uv_queue_work(env->event_loop(), &req, recieveWork, after_recieveWork);
                		} else if (type->Equals(String::NewFromUtf8(env->isolate(), "crypti_response"))) {
                			Local<Value> callback_id = response->Get(String::NewFromUtf8(env->isolate(), "callback_id"));

                			if (callback_id->IsNull() || typeValue->IsUndefined()) {
                				return ThrowError(env->isolate(), "needs callback_id argument");
                			}

                			if (!callback_id->IsNumber()) {
                				return ThrowError(env->isolate(), "callback_id argument should be a number");
                			}

                			Local<Value> responseObj = response->Get(String::NewFromUtf8(env->isolate(), "response"));

                			if (responseObj->IsNull() || responseObj->IsUndefined()) {
                				return ThrowError(env->isolate(), "needs response argument");
                			}

                			if (!responseObj->IsObject()) {
                				return ThrowError(env->isolate(), "response argument should be an object");
                			}

                			Local<Value> errorObj = response->Get(String::NewFromUtf8(env->isolate(), "error"));

                			if (!errorObj->IsNull() && !errorObj->IsUndefined()) {
                				if (!errorObj->IsString()) {
                					return ThrowError(env->isolate(), "response argument should be an string");
                				}
                			}

                			// process response
                			Sandbox_req* request = new Sandbox_req;
                			request->data = (char*)jsonObjects[i].c_str();
                            request->data_length = jsonObjects[i].size();
                			request->isolate = env->isolate();
                			request->callback_id = callback_id->ToNumber()->Value();

                			uv_work_t *req = new uv_work_t;
                           	req->data = request;

                			Sandbox_req *data = ((struct Sandbox_req*)req->data);
                			consoleLog((char*)data->data, data->data_length);

                			// find callback and call
                			uv_queue_work(env->event_loop(), req, findCallback, after_findCallback);
                		} else {
                			return ThrowError(env->isolate(), "unknown call type argument");
                		}
                }
			}
		}

		void StartListen(Environment *env) {
		   uv_pipe_init(env->event_loop(), &stdin_pipe, 0);
		   uv_pipe_open(&stdin_pipe, 3);

		   uv_read_start((uv_stream_t*)&stdin_pipe, alloc_buffer, read_stdin);
		}

		void sendWork(uv_work_t *req) {
			Sandbox_req *data = ((struct Sandbox_req*)req->data);
			write(4, data->data, data->data_length);
		}

		void after_sendWork(uv_work_t *req, int status) {}

		void OnMessageResponse(const FunctionCallbackInfo<Value>& args) {
			Environment* env = Environment::GetCurrent(args.GetIsolate());
			HandleScope scope(env->isolate());

			if (args.Length() < 1) {
				return ThrowError(env->isolate(), "needs argument error");
			}

			Local<Object> response = Object::New(env->isolate());

			if (!args[0]->IsNull() && args[0]->IsString()) {
				Local<String> error = Handle<String>::Cast(args[0]);
				response->Set(String::NewFromUtf8(env->isolate(), "error"), error);
			} else if (!args[0]->IsNull()) {
				return ThrowError(env->isolate(), "error argument should be a string or null");
			} else if (args[0]->IsNull()) {
				if (args.Length() < 2) {
					return ThrowError(env->isolate(), "needs argument error and second agrument response");
				}

				if (!args[1]->IsObject()) {
					return ThrowError(env->isolate(), "error argument should be a object");
				}

				Local<Object> message = Local<Object>::Cast(args[1]);

				response->Set(String::NewFromUtf8(env->isolate(), "response"), message);
			}

			response->Set(String::NewFromUtf8(env->isolate(), "type"), String::NewFromUtf8(env->isolate(), "dapp_response"));

			// get id and find callback
			void* raw = env->isolate()->GetData(0);
			Local<Value> callback_id = Integer::NewFromUnsigned(env->isolate(), (uint32_t) reinterpret_cast<intptr_t>(raw));

			if (callback_id->IsNull()) {
				return ThrowError(env->isolate(), "callback id of response should be provided");
			}

			if (!callback_id->IsNumber()) {
				return ThrowError(env->isolate(), "callback id of response should be a number");
			}

			response->Set(String::NewFromUtf8(env->isolate(), "callback_id"), callback_id);

			uint8_t* buffer = jsonStringify(env->isolate(), response);

			Sandbox_req* request = new Sandbox_req;
			request->data = (char*)buffer;
			request->data_length = strlen((char*)buffer);
			request->isolate = env->isolate();

			uv_work_t* req = new uv_work_t();
			req->data = request;
			uv_queue_work(env->event_loop(), req, sendWork, after_sendWork);
		}

		//////////

		Handle<Object> jsonParse(Isolate* isolate, Handle<String> input) {
//				const int length = input->Utf8Length() + 1;
//				uint8_t* buffer = new uint8_t[length];
//				input->WriteOneByte(buffer, 0, length);
//				consoleLog((char*)buffer, length);

			Local<Object> global = isolate->GetCurrentContext()->Global();
			Local<Object> JSON = global->Get(String::NewFromUtf8(isolate, "JSON"))->ToObject();
			Local<Function> JSON_parse = Handle<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, "parse")));

			Local<Value> parse_args[] = { input };
			return Handle<Object>::Cast(JSON_parse->Call(JSON, 1, parse_args)->ToObject());
		}

		uint8_t* jsonStringify(Isolate* isolate, Handle<Object> input) {
			Local<Object> global = isolate->GetCurrentContext()->Global();
			Local<Object> JSON = global->Get(String::NewFromUtf8(isolate, "JSON"))->ToObject();

			Local<Function> JSON_stringify = Handle<Function>::Cast(JSON->Get(String::NewFromUtf8(isolate, "stringify")));
			Local<Value> stringify_args[] = { input };
			Local<String> str = JSON_stringify->Call(JSON, 1, stringify_args)->ToString();

			const int length = str->Utf8Length() + 1;
			uint8_t* buffer = new uint8_t[length];
			str->WriteOneByte(buffer, 0, length);

			return buffer;
		}

		static void SendMessage(const FunctionCallbackInfo<Value>& args) {
			Environment* env = Environment::GetCurrent(args.GetIsolate());
			HandleScope scope(env->isolate());

			if (args.Length() < 2)
				return ThrowError(env->isolate(), "needs argument object and callback");

			if (!args[1]->IsFunction()) {
				return ThrowError(env->isolate(), "second argument should be a callback");
			}

			if (args[0]->IsObject()) {
				Local<Object> messageCall = Local<Object>::Cast(args[0]);

				if (!messageCall->IsObject()) {
					return ThrowError(env->isolate(), "message argument should be an object");
				}

				Local<Object> messageResponse = Object::New(env->isolate());

				unsigned int cb_id = unique_id++;
				messageResponse->Set(String::NewFromUtf8(env->isolate(), "type"), String::NewFromUtf8(env->isolate(), "dapp_call"));
				messageResponse->Set(String::NewFromUtf8(env->isolate(), "callback_id"), Integer::New(env->isolate(), cb_id)->ToString());
				messageResponse->Set(String::NewFromUtf8(env->isolate(), "message"), messageCall);

				uint8_t* buffer = jsonStringify(env->isolate(), messageResponse);

				SendMessage(env, (char*)buffer, strlen((char*)buffer), cb_id, Handle<Function>::Cast(args[1]));
			} else {
				return ThrowError(env->isolate(), "first argument should be a message object");
			}
		}

		static void OnMessage(const FunctionCallbackInfo<Value>& args) {
			Environment* env = Environment::GetCurrent(args.GetIsolate());
			HandleScope scope(env->isolate());

			if (args.Length() < 1) {
				return ThrowError(env->isolate(), "needs argument object and callback");
			}

			if (!args[0]->IsFunction()) {
				return ThrowError(env->isolate(), "argument should be a callback");
			}

		   pfn.Reset(env->isolate(), args[0].As<Function>());
		}

		void Initialize(Handle<Object> target, Handle<Value> unused, Handle<Context> context) {
			Environment *env = Environment::GetCurrent(context);

			NODE_SET_METHOD(target, "sendMessage", SendMessage);
			NODE_SET_METHOD(target, "onMessage", OnMessage);

			// Start Listen
			StartListen(env);
		}
	}  // namespace Sandbox
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(sandbox, node::Sandbox::Initialize)
