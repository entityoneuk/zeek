// See the file "COPYING" in the main distribution directory for copyright.

#include "zeek-config.h"

#include <ctype.h>

#include <algorithm>

#include "NetVar.h"
#include "Gnutella.h"
#include "Event.h"
#include "analyzer/protocol/pia/PIA.h"
#include "analyzer/Manager.h"

#include "events.bif.h"

using namespace analyzer::gnutella;

GnutellaMsgState::GnutellaMsgState()
	{
	buffer = "";
	current_offset = 0;
	headers = "";
	msg_hops = 0;
	msg_len = 0;
	msg_pos = 0;
	msg_type = 0;
	msg_sent = 1;
	msg_ttl = 0;
	payload_left = 0;
	got_CR = 0;
	payload_len = 0;
	}


Gnutella_Analyzer::Gnutella_Analyzer(Connection* conn)
: tcp::TCP_ApplicationAnalyzer("GNUTELLA", conn)
	{
	state = 0;
	new_state = 0;
	sent_establish = 0;

	ms = 0;

	orig_msg_state = new GnutellaMsgState();
	resp_msg_state = new GnutellaMsgState();
	}

Gnutella_Analyzer::~Gnutella_Analyzer()
	{
	delete orig_msg_state;
	delete resp_msg_state;
	}

void Gnutella_Analyzer::Done()
	{
	tcp::TCP_ApplicationAnalyzer::Done();

	if ( ! sent_establish && (gnutella_establish || gnutella_not_establish) )
		{
		if ( Established() && gnutella_establish )
			EnqueueConnEvent(gnutella_establish, IntrusivePtr{AdoptRef{}, BuildConnVal()});
		else if ( ! Established () && gnutella_not_establish )
			EnqueueConnEvent(gnutella_not_establish, IntrusivePtr{AdoptRef{}, BuildConnVal()});
		}

	if ( gnutella_partial_binary_msg )
		{
		GnutellaMsgState* p = orig_msg_state;

		for ( int i = 0; i < 2; ++i, p = resp_msg_state )
			{
			if ( ! p->msg_sent && p->msg_pos )
				EnqueueConnEvent(gnutella_partial_binary_msg,
					IntrusivePtr{AdoptRef{}, BuildConnVal()},
					make_intrusive<StringVal>(p->msg),
					IntrusivePtr{AdoptRef{}, val_mgr->GetBool((i == 0))},
					IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->msg_pos)}
				);

			else if ( ! p->msg_sent && p->payload_left )
				SendEvents(p, (i == 0));
			}
		}
	}


bool Gnutella_Analyzer::NextLine(const u_char* data, int len)
	{
	if ( ! ms )
		return false;

	if ( Established() || ms->current_offset >= len )
		return false;

	for ( ; ms->current_offset < len; ++ms->current_offset )
		{
		if ( data[ms->current_offset] == '\r' )
			ms->got_CR = 1;

		else if ( data[ms->current_offset] == '\n' && ms->got_CR )
			{
			ms->got_CR = 0;
			++ms->current_offset;
			return true;
			}
		else
			ms->buffer += data[ms->current_offset];
		}

	return false;
	}


bool Gnutella_Analyzer::IsHTTP(string header)
	{
	if ( header.find(" HTTP/1.") == string::npos )
		return false;

	if ( gnutella_http_notify )
		EnqueueConnEvent(gnutella_http_notify, IntrusivePtr{AdoptRef{}, BuildConnVal()});

	analyzer::Analyzer* a = analyzer_mgr->InstantiateAnalyzer("HTTP", Conn());

	if ( a && Parent()->AddChildAnalyzer(a) )
		{
		if ( Parent()->IsAnalyzer("TCP") )
			{
			// Replay buffered data.
			pia::PIA* pia = static_cast<analyzer::TransportLayerAnalyzer *>(Parent())->GetPIA();
			if ( pia )
				static_cast<pia::PIA_TCP *>(pia)->ReplayStreamBuffer(a);
			}

		Parent()->RemoveChildAnalyzer(this);
		}

	return true;
	}


bool Gnutella_Analyzer::GnutellaOK(string header)
	{
	if ( strncmp("GNUTELLA", header.data(), 8) )
		return false;

	int codepos = header.find(' ') + 1;
	if ( ! strncmp("200", header.data() + codepos, 3) )
		return true;

	return false;
	}


void Gnutella_Analyzer::DeliverLines(int len, const u_char* data, bool orig)
	{
	if ( ! ms )
		return;

	while ( NextLine(data, len) )
		{
		if ( ms->buffer.length() )
			{
			if ( ms->headers.length() == 0 )
				{
				if ( IsHTTP(ms->buffer) )
					return;
				if ( GnutellaOK(ms->buffer) )
					new_state |=
						orig ? ORIG_OK : RESP_OK;
				}

			ms->headers = ms->headers + "\r\n" + ms->buffer;
			ms->buffer = "";
			}
		else
			{
			if ( gnutella_text_msg )
				EnqueueConnEvent(gnutella_text_msg,
					IntrusivePtr{AdoptRef{}, BuildConnVal()},
					IntrusivePtr{AdoptRef{}, val_mgr->GetBool(orig)},
					make_intrusive<StringVal>(ms->headers.data())
				);

			ms->headers = "";
			state |= new_state;

			if ( Established () && gnutella_establish )
				{
				sent_establish = 1;

				EnqueueConnEvent(gnutella_establish, IntrusivePtr{AdoptRef{}, BuildConnVal()});
				}
			}
		}
	}

void Gnutella_Analyzer::DissectMessage(char* msg)
	{
	if ( ! ms )
		return;

	ms->msg_type = msg[16];
	ms->msg_ttl = msg[17];
	ms->msg_hops = msg[18];

	memcpy(&ms->msg_len, &msg[19], 4);
	}


void Gnutella_Analyzer::SendEvents(GnutellaMsgState* p, bool is_orig)
	{
	if ( p->msg_sent )
		return;

	if ( gnutella_binary_msg )
		EnqueueConnEvent(gnutella_binary_msg,
			IntrusivePtr{AdoptRef{}, BuildConnVal()},
			IntrusivePtr{AdoptRef{}, val_mgr->GetBool(is_orig)},
			IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->msg_type)},
			IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->msg_ttl)},
			IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->msg_hops)},
			IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->msg_len)},
			make_intrusive<StringVal>(p->payload),
			IntrusivePtr{AdoptRef{}, val_mgr->GetCount(p->payload_len)},
			IntrusivePtr{AdoptRef{}, val_mgr->GetBool((p->payload_len < min(p->msg_len, (unsigned int)GNUTELLA_MAX_PAYLOAD)))},
			IntrusivePtr{AdoptRef{}, val_mgr->GetBool((p->payload_left == 0))}
		);
	}


void Gnutella_Analyzer::DeliverMessages(int len, const u_char* data, bool orig)
	{
	if ( ! ms )
		return;

	while ( ms->current_offset < len )
		{
		ms->msg_sent = 0;

		unsigned int bytes_left = len - ms->current_offset;
		unsigned int needed = 0;

		if ( ms->msg_pos )
			needed = GNUTELLA_MSG_SIZE - ms->msg_pos;

		if ( (! ms->msg_pos && ! ms->payload_left &&
		      (bytes_left >= GNUTELLA_MSG_SIZE)) ||
		     (ms->msg_pos && (bytes_left >= needed)) )
			{
			int sz = ms->msg_pos ? needed : GNUTELLA_MSG_SIZE;

			memcpy(&ms->msg[ms->msg_pos],
				&data[ms->current_offset], sz);

			ms->current_offset += sz;
			DissectMessage(ms->msg);
			ms->payload_left = ms->msg_len;
			ms->msg_pos = 0;
			if ( ms->msg_len == 0 )
				SendEvents(ms, orig);
			}

		else if ( (! ms->msg_pos && ! ms->payload_left &&
			   (bytes_left < GNUTELLA_MSG_SIZE)) ||
			  (ms->msg_pos && (bytes_left < needed)) )
			{
			memcpy(&ms->msg[ms->msg_pos], &data[ms->current_offset],
				bytes_left);
			ms->current_offset += bytes_left;
			ms->msg_pos += bytes_left;
			}

		else if ( ms->payload_left )
			{
			unsigned int space =
				ms->payload_len >= GNUTELLA_MAX_PAYLOAD ?
					0 : GNUTELLA_MAX_PAYLOAD - ms->payload_len;
			unsigned int sz =
				(bytes_left < space) ? bytes_left : space;

			if ( space )
				{
				memcpy(&ms->payload[ms->payload_len],
					&data[ms->current_offset], sz);
				ms->payload_len += sz;
				}

			if ( ms->payload_left > bytes_left )
				{
				ms->current_offset += bytes_left;
				ms->payload_left -= bytes_left;
				}
			else
				{
				ms->current_offset += ms->payload_left;
				ms->payload_left = 0;
				SendEvents(ms, orig);
				}
			}
		}
	}


void Gnutella_Analyzer::DeliverStream(int len, const u_char* data, bool orig)
	{
	tcp::TCP_ApplicationAnalyzer::DeliverStream(len, data, orig);

	ms = orig ? orig_msg_state : resp_msg_state;
	ms->current_offset = 0;
	if ( ! Established() )
		{
		DeliverLines(len, data, orig);

		if ( Established() && ms->current_offset < len &&
			  gnutella_binary_msg )
			DeliverMessages(len, data, orig);
		}

	else if ( gnutella_binary_msg )
		DeliverMessages(len, data, orig);
	}
