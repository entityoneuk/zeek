// See the file "COPYING" in the main distribution directory for copyright.

#include "ReaderBackend.h"
#include "ReaderFrontend.h"
#include "Manager.h"

using threading::Value;
using threading::Field;

namespace input {

class PutMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	PutMessage(ReaderFrontend* reader, Value* *val)
		: threading::OutputMessage<ReaderFrontend>("Put", reader),
		val(val) {}

	bool Process() override
		{
		input_mgr->Put(Object(), val);
		return true;
		}

private:
	Value* *val;
};

class DeleteMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	DeleteMessage(ReaderFrontend* reader, Value* *val)
		: threading::OutputMessage<ReaderFrontend>("Delete", reader),
		val(val) {}

	bool Process() override
		{
		return input_mgr->Delete(Object(), val);
		}

private:
	Value* *val;
};

class ClearMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	ClearMessage(ReaderFrontend* reader)
		: threading::OutputMessage<ReaderFrontend>("Clear", reader) {}

	bool Process() override
		{
		input_mgr->Clear(Object());
		return true;
		}

private:
};

class SendEventMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	SendEventMessage(ReaderFrontend* reader, const char* name, const int num_vals, Value* *val)
		: threading::OutputMessage<ReaderFrontend>("SendEvent", reader),
		name(copy_string(name)), num_vals(num_vals), val(val) {}

	~SendEventMessage() override	{ delete [] name; }

	bool Process() override
		{
		bool success = input_mgr->SendEvent(Object(), name, num_vals, val);

		if ( ! success )
			reporter->Error("SendEvent for event %s failed", name);

		return true; // We do not want to die if sendEvent fails because the event did not return.
		}

private:
	const char* name;
	const int num_vals;
	Value* *val;
};

class ReaderErrorMessage final : public threading::OutputMessage<ReaderFrontend>
{
public:
	enum Type {
		INFO, WARNING, ERROR
	};

	ReaderErrorMessage(ReaderFrontend* reader, Type arg_type, const char* arg_msg)
		: threading::OutputMessage<ReaderFrontend>("ReaderErrorMessage", reader)
		{ type = arg_type; msg = copy_string(arg_msg); }

	~ReaderErrorMessage() override 	 { delete [] msg; }

	bool Process() override;

private:
	const char* msg;
	Type type;
};

class SendEntryMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	SendEntryMessage(ReaderFrontend* reader, Value* *val)
		: threading::OutputMessage<ReaderFrontend>("SendEntry", reader),
		val(val) { }

	bool Process() override
		{
		input_mgr->SendEntry(Object(), val);
		return true;
		}

private:
	Value* *val;
};

class EndCurrentSendMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	EndCurrentSendMessage(ReaderFrontend* reader)
		: threading::OutputMessage<ReaderFrontend>("EndCurrentSend", reader) {}

	bool Process() override
		{
		input_mgr->EndCurrentSend(Object());
		return true;
		}

private:
};

class EndOfDataMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	EndOfDataMessage(ReaderFrontend* reader)
		: threading::OutputMessage<ReaderFrontend>("EndOfData", reader) {}

	bool Process() override
		{
		input_mgr->SendEndOfData(Object());
		return true;
		}

private:
};

class ReaderClosedMessage final : public threading::OutputMessage<ReaderFrontend> {
public:
	ReaderClosedMessage(ReaderFrontend* reader)
		: threading::OutputMessage<ReaderFrontend>("ReaderClosed", reader) {}

	bool Process() override
		{
		Object()->SetDisable();
		return input_mgr->RemoveStreamContinuation(Object());
		}

private:
};

class DisableMessage final : public threading::OutputMessage<ReaderFrontend>
{
public:
	DisableMessage(ReaderFrontend* writer)
		: threading::OutputMessage<ReaderFrontend>("Disable", writer)	{}

	bool Process() override
		{
		Object()->SetDisable();
		// And - because we do not need disabled objects any more -
		// there is no way to re-enable them, so simply delete them.
		// This avoids the problem of having to periodically check if
		// there are any disabled readers out there. As soon as a
		// reader disables itself, it deletes itself.
		input_mgr->RemoveStream(Object());
		return true;
		}
};

bool ReaderErrorMessage::Process()
	{
	switch ( type ) {

	case INFO:
		input_mgr->Info(Object(), msg);
		break;

	case WARNING:
		input_mgr->Warning(Object(), msg);
		break;

	case ERROR:
		input_mgr->Error(Object(), msg);
		break;
	}

	return true;
	}


using namespace input;

ReaderBackend::ReaderBackend(ReaderFrontend* arg_frontend) : MsgThread()
	{
	disabled = true; // disabled will be set correcty in init.
	frontend = arg_frontend;
	info = new ReaderInfo(frontend->Info());
	num_fields = 0;
	fields = 0;

	SetName(frontend->Name());
	}

ReaderBackend::~ReaderBackend()
	{
	delete info;
	}

void ReaderBackend::Put(Value* *val)
	{
	SendOut(new PutMessage(frontend, val));
	}

void ReaderBackend::Delete(Value* *val)
	{
	SendOut(new DeleteMessage(frontend, val));
	}

void ReaderBackend::Clear()
	{
	SendOut(new ClearMessage(frontend));
	}

void ReaderBackend::SendEvent(const char* name, const int num_vals, Value* *vals)
	{
	SendOut(new SendEventMessage(frontend, name, num_vals, vals));
	}

void ReaderBackend::EndCurrentSend()
	{
	SendOut(new EndCurrentSendMessage(frontend));
	}

void ReaderBackend::EndOfData()
	{
	SendOut(new EndOfDataMessage(frontend));
	}

void ReaderBackend::SendEntry(Value* *vals)
	{
	SendOut(new SendEntryMessage(frontend, vals));
	}

bool ReaderBackend::Init(const int arg_num_fields,
		         const threading::Field* const* arg_fields)
	{
	if ( Failed() )
		return true;

	disabled = false;

	SetOSName(Fmt("zk.%s", Name()));

	num_fields = arg_num_fields;
	fields = arg_fields;

	// disable if DoInit returns error.
	int success = DoInit(*info, arg_num_fields, arg_fields);

	if ( ! success )
		{
		Error("Init failed");
		DisableFrontend();
		}

	return success;
	}

bool ReaderBackend::OnFinish(double network_time)
	{
	if ( ! Failed() )
		DoClose();

	disabled = true; // frontend disables itself when it gets the Close-message.
	SendOut(new ReaderClosedMessage(frontend));

	if ( fields != 0 )
		{
		for ( unsigned int i = 0; i < num_fields; i++ )
			delete(fields[i]);

		delete [] (fields);
		fields = 0;
		}

	return true;
	}

bool ReaderBackend::Update()
	{
	if ( disabled )
		return false;

	if ( Failed() )
		return true;

	bool success = DoUpdate();
	if ( ! success )
		DisableFrontend();

	return ! disabled; // always return failure if we have been disabled in the meantime
	}

void ReaderBackend::DisableFrontend()
	{
	// We might already have been disabled - e.g., due to a call to
	// error. In that case, ignore this...
	if ( disabled )
		return;

	// We also set disabled here, because there still may be other
	// messages queued and we will dutifully ignore these from now.
	disabled = true;
	SendOut(new DisableMessage(frontend));
	}

bool ReaderBackend::OnHeartbeat(double network_time, double current_time)
	{
	if ( Failed() )
		return true;

	return DoHeartbeat(network_time, current_time);
	}

void ReaderBackend::Info(const char* msg)
	{
	SendOut(new ReaderErrorMessage(frontend, ReaderErrorMessage::INFO, msg));
	MsgThread::Info(msg);
	}

void ReaderBackend::FailWarn(bool is_error, const char *msg, bool suppress_future)
	{
	if ( is_error )
		Error(msg);
	else
		{
		// suppress error message when we are already in error mode.
		// There is no reason to repeat it every second.
		if ( ! suppress_warnings )
			Warning(msg);

		if ( suppress_future )
			suppress_warnings = true;
		}
	}
void ReaderBackend::Warning(const char* msg)
	{
	if ( suppress_warnings )
		return;

	SendOut(new ReaderErrorMessage(frontend, ReaderErrorMessage::WARNING, msg));
	MsgThread::Warning(msg);
	}

void ReaderBackend::Error(const char* msg)
	{
	SendOut(new ReaderErrorMessage(frontend, ReaderErrorMessage::ERROR, msg));
	MsgThread::Error(msg);

	// Force errors to be fatal.
	DisableFrontend();
	}

}
