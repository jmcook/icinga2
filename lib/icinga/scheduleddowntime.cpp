/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2015 Icinga Development Team (http://www.icinga.org)    *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "icinga/scheduleddowntime.hpp"
#include "icinga/scheduleddowntime.tcpp"
#include "icinga/legacytimeperiod.hpp"
#include "icinga/downtime.hpp"
#include "icinga/service.hpp"
#include "base/timer.hpp"
#include "base/dynamictype.hpp"
#include "base/initialize.hpp"
#include "base/utility.hpp"
#include "base/objectlock.hpp"
#include "base/convert.hpp"
#include "base/logger.hpp"
#include "base/exception.hpp"
#include <boost/foreach.hpp>

using namespace icinga;

REGISTER_TYPE(ScheduledDowntime);

INITIALIZE_ONCE(&ScheduledDowntime::StaticInitialize);

static Timer::Ptr l_Timer;

String ScheduledDowntimeNameComposer::MakeName(const String& shortName, const Object::Ptr& context) const
{
	ScheduledDowntime::Ptr downtime = dynamic_pointer_cast<ScheduledDowntime>(context);

	if (!downtime)
		return "";

	String name = downtime->GetHostName();

	if (!downtime->GetServiceName().IsEmpty())
		name += "!" + downtime->GetServiceName();

	name += "!" + shortName;

	return name;
}

void ScheduledDowntime::StaticInitialize(void)
{
	l_Timer = new Timer();
	l_Timer->SetInterval(60);
	l_Timer->OnTimerExpired.connect(boost::bind(&ScheduledDowntime::TimerProc));
	l_Timer->Start();
}

void ScheduledDowntime::OnAllConfigLoaded(void)
{
	CustomVarObject::OnAllConfigLoaded();

	if (!GetCheckable())
		BOOST_THROW_EXCEPTION(ScriptError("ScheduledDowntime '" + GetName() + "' references a host/service which doesn't exist.", GetDebugInfo()));
}

void ScheduledDowntime::Start(void)
{
	DynamicObject::Start();

	CreateNextDowntime();
}

void ScheduledDowntime::TimerProc(void)
{
	BOOST_FOREACH(const ScheduledDowntime::Ptr& sd, DynamicType::GetObjectsByType<ScheduledDowntime>()) {
		sd->CreateNextDowntime();
	}
}

Checkable::Ptr ScheduledDowntime::GetCheckable(void) const
{
	Host::Ptr host = Host::GetByName(GetHostName());

	if (GetServiceName().IsEmpty())
		return host;
	else
		return host->GetServiceByShortName(GetServiceName());
}

std::pair<double, double> ScheduledDowntime::FindNextSegment(void)
{
	time_t refts = Utility::GetTime();
	tm reference = Utility::LocalTime(refts);

	Log(LogDebug, "ScheduledDowntime")
	    << "Finding next scheduled downtime segment for time " << refts;

	Dictionary::Ptr ranges = GetRanges();

	Array::Ptr segments = new Array();

	Dictionary::Ptr bestSegment;
	double bestBegin;
	double now = Utility::GetTime();

	ObjectLock olock(ranges);
	BOOST_FOREACH(const Dictionary::Pair& kv, ranges) {
		Log(LogDebug, "ScheduledDowntime")
		    << "Evaluating segment: " << kv.first << ": " << kv.second << " at ";

		Dictionary::Ptr segment = LegacyTimePeriod::FindNextSegment(kv.first, kv.second, &reference);

		if (!segment)
			continue;

		Log(LogDebug, "ScheduledDowntime")
		    << "Considering segment: " << Utility::FormatDateTime("%c", segment->Get("begin")) << " -> " << Utility::FormatDateTime("%c", segment->Get("end"));

		double begin = segment->Get("begin");

		if (begin < now)
			continue;

		if (!bestSegment || begin < bestBegin) {
			bestSegment = segment;
			bestBegin = begin;
		}
	}

	if (bestSegment)
		return std::make_pair(bestSegment->Get("begin"), bestSegment->Get("end"));
	else
		return std::make_pair(0, 0);
}

void ScheduledDowntime::CreateNextDowntime(void)
{
	Dictionary::Ptr downtimes = GetCheckable()->GetDowntimes();

	{
		ObjectLock dlock(downtimes);
		BOOST_FOREACH(const Dictionary::Pair& kv, downtimes) {
			Downtime::Ptr downtime = kv.second;

			if (downtime->GetScheduledBy() != GetName() ||
			    downtime->GetStartTime() < Utility::GetTime())
				continue;

			/* We've found a downtime that is owned by us and that hasn't started yet - we're done. */
			return;
		}
	}

	std::pair<double, double> segment = FindNextSegment();

	if (segment.first == 0 && segment.second == 0) {
		tm reference = Utility::LocalTime(Utility::GetTime());
		reference.tm_mday++;
		reference.tm_hour = 0;
		reference.tm_min = 0;
		reference.tm_sec = 0;

		return;
	}

	String uid = GetCheckable()->AddDowntime(GetAuthor(), GetComment(),
	    segment.first, segment.second,
	    GetFixed(), String(), GetDuration(), GetName());

	Downtime::Ptr downtime = Checkable::GetDowntimeByID(uid);
	downtime->SetConfigOwner(GetName());
}

void ScheduledDowntime::ValidateRanges(const Dictionary::Ptr& value, const ValidationUtils& utils)
{
	if (!value)
		return;

	/* create a fake time environment to validate the definitions */
	time_t refts = Utility::GetTime();
	tm reference = Utility::LocalTime(refts);
	Array::Ptr segments = new Array();

	ObjectLock olock(value);
	BOOST_FOREACH(const Dictionary::Pair& kv, value) {
		try {
			tm begin_tm, end_tm;
			int stride;
			LegacyTimePeriod::ParseTimeRange(kv.first, &begin_tm, &end_tm, &stride, &reference);
		} catch (const std::exception& ex) {
			BOOST_THROW_EXCEPTION(ValidationError(this, boost::assign::list_of("ranges"), "Invalid time specification '" + kv.first + "': " + ex.what()));
		}

		try {
			LegacyTimePeriod::ProcessTimeRanges(kv.second, &reference, segments);
		} catch (const std::exception& ex) {
			BOOST_THROW_EXCEPTION(ValidationError(this, boost::assign::list_of("ranges"), "Invalid time range definition '" + kv.second + "': " + ex.what()));
		}
	}
}
