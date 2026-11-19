#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rfid_sample_protocol.h"

#define ARRAY_SIZE(values) (sizeof(values) / sizeof((values)[0]))
#define CAPTURE_CAPACITY 8U

/* The checksum bytes below are arbitrary, not verified checksum examples. */
static const uint8_t without_time[] = {
	0x1b, 0x39, 0x01, 0xff, 0x0a, 0x00, 0x08, 0x53, 0x10,
	0x21, 0x11, 0x00, 0x86, 0x04, 0x00, 0x00, 0xa5,
};

static const uint8_t with_time[] = {
	0x1b, 0x39, 0x01, 0xff, 0x12, 0x00, 0x08, 0x53, 0x10,
	0x21, 0x11, 0x00, 0x86, 0x04, 0x00, 0x16, 0x0c, 0x1a,
	0x09, 0x14, 0x39, 0x00, 0x00, 0x00, 0xa5,
};

struct capture {
	struct rfid_sample_tag_record records[CAPTURE_CAPACITY];
	size_t count;
};

struct response_capture {
	struct rfid_sample_response responses[CAPTURE_CAPACITY];
	size_t count;
};

static void capture_response(const struct rfid_sample_response *response,
			     void *user_data)
{
	struct response_capture *capture = user_data;

	assert(response != NULL);
	assert(capture != NULL);
	assert(capture->count < ARRAY_SIZE(capture->responses));
	capture->responses[capture->count++] = *response;
}

static void capture_record(const struct rfid_sample_tag_record *record, void *user_data)
{
	struct capture *capture = user_data;

	assert(record != NULL);
	assert(capture != NULL);
	assert(capture->count < ARRAY_SIZE(capture->records));
	capture->records[capture->count++] = *record;
}

static struct rfid_sample_protocol_stats get_stats(const struct rfid_sample_protocol *protocol)
{
	struct rfid_sample_protocol_stats stats;

	rfid_sample_protocol_get_stats(protocol, &stats);
	return stats;
}

static void assert_stats_equal(const struct rfid_sample_protocol_stats *left,
			       const struct rfid_sample_protocol_stats *right)
{
	assert(left->frames == right->frames);
	assert(left->noise_bytes == right->noise_bytes);
	assert(left->sync_losses == right->sync_losses);
	assert(left->length_errors == right->length_errors);
	assert(left->tag_length_errors == right->tag_length_errors);
	assert(left->unsupported_messages == right->unsupported_messages);
	assert(left->partial_timeouts == right->partial_timeouts);
	assert(left->time_field_errors == right->time_field_errors);
	assert(left->checksum_errors == right->checksum_errors);
}

static void assert_record(const struct rfid_sample_tag_record *record,
			  const uint8_t *frame, size_t size)
{
	bool timed = size == sizeof(with_time);
	uint32_t uid = ((uint32_t)frame[10] << 24) |
		       ((uint32_t)frame[11] << 16) |
		       ((uint32_t)frame[12] << 8) | (uint32_t)frame[13];

	assert(record->frame_size == size);
	assert(record->uid == uid);
	assert(record->address == frame[3]);
	assert(record->rssi == frame[7]);
	assert(record->pc == frame[8]);
	assert(record->battery == frame[9]);
	assert(record->antenna == frame[14]);
	assert(record->additional_type == frame[size - 2U]);
	assert(record->checksum == frame[size - 1U]);
	assert(!record->checksum_verified);
	assert(record->has_device_time == timed);
	assert(memcmp(record->raw_frame, frame, size) == 0);
	if (timed) {
		assert(memcmp(record->device_time, frame + 15U,
			      sizeof(record->device_time)) == 0);
	} else {
		assert(!record->time_fields_valid);
		for (size_t i = 0U; i < sizeof(record->device_time); i++) {
			assert(record->device_time[i] == 0U);
		}
	}
}

static void test_complete_frames(void)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};
	struct rfid_sample_protocol_stats stats;

	assert(sizeof(without_time) == 17U);
	assert(sizeof(with_time) == 25U);
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 0U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, with_time, sizeof(with_time), 1U,
			   capture_record, &capture);
	assert(capture.count == 2U);
	assert_record(&capture.records[0], without_time, sizeof(without_time));
	assert_record(&capture.records[1], with_time, sizeof(with_time));
	assert(capture.records[0].uid == UINT32_C(0x11008604));
	assert(capture.records[1].time_fields_valid);
	stats = get_stats(&protocol);
	assert(stats.frames == 2U);
	assert(stats.noise_bytes == 0U);
	assert(stats.sync_losses == 0U);
	assert(stats.time_field_errors == 0U);
}

static void test_every_fragment_boundary(const uint8_t *frame, size_t size)
{
	for (size_t split = 1U; split < size; split++) {
		struct rfid_sample_protocol protocol;
		struct capture capture = {0};

		rfid_sample_protocol_init(&protocol, 0U);
		rfid_sample_protocol_feed(&protocol, frame, split, 100U,
				   capture_record, &capture);
		assert(capture.count == 0U);
		rfid_sample_protocol_feed(&protocol, frame + split, size - split, 101U,
				   capture_record, &capture);
		assert(capture.count == 1U);
		assert_record(&capture.records[0], frame, size);
	}
}

static void test_bytewise(const uint8_t *frame, size_t size)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	for (size_t i = 0U; i < size; i++) {
		rfid_sample_protocol_feed(&protocol, frame + i, 1U, (uint64_t)i,
				   capture_record, &capture);
		assert(capture.count == ((i + 1U == size) ? 1U : 0U));
	}
	assert_record(&capture.records[0], frame, size);
}

static void test_concatenated_frames(void)
{
	uint8_t stream[sizeof(without_time) + sizeof(with_time) + sizeof(without_time)];
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	memcpy(stream, without_time, sizeof(without_time));
	memcpy(stream + sizeof(without_time), with_time, sizeof(with_time));
	memcpy(stream + sizeof(without_time) + sizeof(with_time), without_time,
	       sizeof(without_time));
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, stream, sizeof(stream), 10U,
			   capture_record, &capture);
	assert(capture.count == 3U);
	assert_record(&capture.records[0], without_time, sizeof(without_time));
	assert_record(&capture.records[1], with_time, sizeof(with_time));
	assert_record(&capture.records[2], without_time, sizeof(without_time));
	assert(get_stats(&protocol).frames == 3U);
}

static void test_noise_and_overlapping_sync(void)
{
	static const uint8_t prefixes[][4] = {
		{0x00, 0xff, 0x7e, 0x55},
		{0x00, 0x00, 0x00, 0x1b},
		{0x00, 0x00, 0x1b, 0x39},
		{0x1b, 0x39, 0x01, 0xff},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(prefixes); i++) {
		uint8_t stream[sizeof(prefixes[0]) + sizeof(without_time)];
		struct rfid_sample_protocol protocol;
		struct capture capture = {0};

		memcpy(stream, prefixes[i], sizeof(prefixes[i]));
		memcpy(stream + sizeof(prefixes[i]), without_time,
		       sizeof(without_time));
		rfid_sample_protocol_init(&protocol, 0U);
		rfid_sample_protocol_feed(&protocol, stream, sizeof(stream), 1U,
				   capture_record, &capture);
		assert(capture.count == 1U);
		assert_record(&capture.records[0], without_time, sizeof(without_time));
		assert(get_stats(&protocol).noise_bytes == sizeof(prefixes[i]));
	}
}

static struct rfid_sample_protocol_stats reject_then_recover(const uint8_t *bad,
						      size_t size)
{
	uint8_t stream[RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE + sizeof(without_time)];
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};
	struct rfid_sample_protocol_stats stats;

	assert(size <= RFID_SAMPLE_PROTOCOL_MAX_FRAME_SIZE);
	memcpy(stream, bad, size);
	memcpy(stream + size, without_time, sizeof(without_time));
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, stream, size + sizeof(without_time), 0U,
			   capture_record, &capture);
	assert(capture.count == 1U);
	assert_record(&capture.records[0], without_time, sizeof(without_time));
	stats = get_stats(&protocol);
	assert(stats.frames == 1U);
	assert(stats.sync_losses >= 1U);
	assert(stats.noise_bytes >= size);
	return stats;
}

static void test_wrong_message_type(void)
{
	uint8_t bad[sizeof(without_time)];
	struct rfid_sample_protocol_stats stats;

	memcpy(bad, without_time, sizeof(bad));
	bad[2] = 0x02U;
	stats = reject_then_recover(bad, sizeof(bad));
	assert(stats.unsupported_messages == 1U);
	memcpy(bad, without_time, sizeof(bad));
	bad[1] = 0x38U;
	stats = reject_then_recover(bad, sizeof(bad));
	assert(stats.frames == 1U);
}

static void test_invalid_packet_lengths(void)
{
	static const uint16_t lengths[] = {0U, 9U, 11U, 17U, 19U, 0x0a00U, 0xffffU};

	for (size_t i = 0U; i < ARRAY_SIZE(lengths); i++) {
		uint8_t bad[sizeof(without_time)];
		struct rfid_sample_protocol_stats stats;

		memcpy(bad, without_time, sizeof(bad));
		bad[4] = (uint8_t)(lengths[i] & 0xffU);
		bad[5] = (uint8_t)(lengths[i] >> 8);
		stats = reject_then_recover(bad, sizeof(bad));
		assert(stats.length_errors == 1U);
	}
}

static void test_invalid_tag_lengths(void)
{
	static const uint8_t lengths[] = {0U, 7U, 9U, 0xffU};

	for (size_t i = 0U; i < ARRAY_SIZE(lengths); i++) {
		uint8_t bad[sizeof(without_time)];
		struct rfid_sample_protocol_stats stats;

		memcpy(bad, without_time, sizeof(bad));
		bad[6] = lengths[i];
		stats = reject_then_recover(bad, sizeof(bad));
		assert(stats.tag_length_errors == 1U);
	}
}

static void test_tag_length_rejection_preserves_next_header(void)
{
	static const uint8_t partial[] = {0x1b, 0x39, 0x01, 0xff, 0x0a, 0x00};
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, partial, sizeof(partial), 0U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 1U,
			   capture_record, &capture);
	assert(capture.count == 1U);
	assert_record(&capture.records[0], without_time, sizeof(without_time));
	assert(get_stats(&protocol).tag_length_errors == 1U);
	assert(get_stats(&protocol).noise_bytes == sizeof(partial));
}

static void test_checksum_is_not_verified(void)
{
	static const uint8_t checksums[] = {0x00U, 0xa5U, 0xffU};
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	for (size_t i = 0U; i < ARRAY_SIZE(checksums); i++) {
		uint8_t frame[sizeof(without_time)];

		memcpy(frame, without_time, sizeof(frame));
		frame[sizeof(frame) - 1U] = checksums[i];
		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), (uint64_t)i,
				   capture_record, &capture);
		assert(capture.count == i + 1U);
		assert_record(&capture.records[i], frame, sizeof(frame));
		assert(!capture.records[i].checksum_verified);
	}
	for (size_t i = 0U; i < ARRAY_SIZE(checksums); i++) {
		uint8_t frame[sizeof(with_time)];
		size_t output = ARRAY_SIZE(checksums) + i;

		memcpy(frame, with_time, sizeof(frame));
		frame[sizeof(frame) - 1U] = checksums[i];
		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), (uint64_t)output,
				   capture_record, &capture);
		assert(capture.count == output + 1U);
		assert_record(&capture.records[output], frame, sizeof(frame));
		assert(!capture.records[output].checksum_verified);
	}
	assert(get_stats(&protocol).frames == 2U * ARRAY_SIZE(checksums));
}

static void test_raw_fields_and_big_endian_uid(void)
{
	uint8_t frame[sizeof(with_time)];
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	memcpy(frame, with_time, sizeof(frame));
	frame[3] = 0x42U;
	frame[7] = 0xfeU;
	frame[8] = 0xf0U;
	frame[9] = 0x18U;
	frame[10] = 0x80U;
	frame[11] = 0x12U;
	frame[12] = 0x34U;
	frame[13] = 0xabU;
	frame[14] = 0x07U;
	frame[15] = 0xffU;
	frame[21] = 0xfeU;
	frame[22] = 0xdcU;
	frame[23] = 0x7fU;
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), 0U,
			   capture_record, &capture);
	assert(capture.count == 1U);
	assert_record(&capture.records[0], frame, sizeof(frame));
	assert(capture.records[0].uid == UINT32_C(0x801234ab));
	assert(capture.records[0].time_fields_valid);
}

static void test_invalid_time_is_retained(void)
{
	static const struct {
		size_t offset;
		uint8_t value;
	} invalid_fields[] = {
		{16U, 0U}, {16U, 13U}, {17U, 0U}, {17U, 32U},
		{18U, 24U}, {19U, 60U}, {20U, 60U},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(invalid_fields); i++) {
		uint8_t frame[sizeof(with_time)];
		struct rfid_sample_protocol protocol;
		struct capture capture = {0};

		memcpy(frame, with_time, sizeof(frame));
		frame[invalid_fields[i].offset] = invalid_fields[i].value;
		rfid_sample_protocol_init(&protocol, 0U);
		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), 0U,
				   capture_record, &capture);
		assert(capture.count == 1U);
		assert_record(&capture.records[0], frame, sizeof(frame));
		assert(!capture.records[0].time_fields_valid);
		assert(get_stats(&protocol).time_field_errors == 1U);
	}
}

static void test_timeout_boundary_and_recovery(void)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};
	struct rfid_sample_protocol_stats stats;

	rfid_sample_protocol_init(&protocol, 100U);
	rfid_sample_protocol_feed(&protocol, without_time, 8U, 0U, capture_record, &capture);
	rfid_sample_protocol_expire(&protocol, 99U);
	assert(get_stats(&protocol).partial_timeouts == 0U);
	rfid_sample_protocol_expire(&protocol, 100U);
	stats = get_stats(&protocol);
	assert(stats.partial_timeouts == 1U);
	assert(stats.noise_bytes == 8U);
	assert(stats.sync_losses == 1U);
	assert(capture.count == 0U);
	rfid_sample_protocol_expire(&protocol, 101U);
	assert(get_stats(&protocol).partial_timeouts == 1U);
	rfid_sample_protocol_feed(&protocol, without_time + 8U, sizeof(without_time) - 8U,
			   102U, capture_record, &capture);
	assert(capture.count == 0U);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 103U,
			   capture_record, &capture);
	assert(capture.count == 1U);
}

static void test_total_age_and_zero_length_poll(void)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 100U);
	rfid_sample_protocol_feed(&protocol, with_time, 3U, 0U, capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, with_time + 3U, 6U, 90U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, NULL, 0U, 100U, capture_record, &capture);
	assert(get_stats(&protocol).partial_timeouts == 1U);
	assert(capture.count == 0U);
	rfid_sample_protocol_feed(&protocol, with_time, sizeof(with_time), 101U,
			   capture_record, &capture);
	assert(capture.count == 1U);
}

static void test_resync_preserves_original_arrival_age(void)
{
	static const uint8_t partial[] = {0x1b, 0x39, 0x01, 0xff, 0x1b};
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 100U);
	rfid_sample_protocol_feed(&protocol, partial, sizeof(partial), 0U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, without_time + 1U, 5U, 80U,
			   capture_record, &capture);
	assert(get_stats(&protocol).length_errors == 1U);
	rfid_sample_protocol_expire(&protocol, 99U);
	assert(get_stats(&protocol).partial_timeouts == 0U);
	rfid_sample_protocol_expire(&protocol, 100U);
	assert(get_stats(&protocol).partial_timeouts == 1U);
	assert(capture.count == 0U);
}

static void test_backwards_clock(void)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};

	rfid_sample_protocol_init(&protocol, 100U);
	rfid_sample_protocol_feed(&protocol, without_time, 7U, 200U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 199U,
			   capture_record, &capture);
	assert(get_stats(&protocol).partial_timeouts == 1U);
	assert(capture.count == 1U);
	assert_record(&capture.records[0], without_time, sizeof(without_time));
}

static void test_default_timeout(void)
{
	struct rfid_sample_protocol protocol;

	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, without_time, 1U, 50U, NULL, NULL);
	rfid_sample_protocol_expire(&protocol, 50U + RFID_SAMPLE_PROTOCOL_DEFAULT_FRAME_TIMEOUT_MS - 1U);
	assert(get_stats(&protocol).partial_timeouts == 0U);
	rfid_sample_protocol_expire(&protocol, 50U + RFID_SAMPLE_PROTOCOL_DEFAULT_FRAME_TIMEOUT_MS);
	assert(get_stats(&protocol).partial_timeouts == 1U);
}

static void test_instance_isolation(void)
{
	struct rfid_sample_protocol first;
	struct rfid_sample_protocol second;
	struct capture first_capture = {0};
	struct capture second_capture = {0};

	rfid_sample_protocol_init(&first, 100U);
	rfid_sample_protocol_init(&second, 500U);
	rfid_sample_protocol_feed(&first, without_time, 8U, 0U,
			   capture_record, &first_capture);
	rfid_sample_protocol_feed(&second, with_time, 12U, 0U,
			   capture_record, &second_capture);
	rfid_sample_protocol_expire(&first, 100U);
	rfid_sample_protocol_feed(&second, with_time + 12U, sizeof(with_time) - 12U,
			   100U, capture_record, &second_capture);
	assert(first_capture.count == 0U);
	assert(second_capture.count == 1U);
	assert(get_stats(&first).partial_timeouts == 1U);
	assert(get_stats(&second).partial_timeouts == 0U);
	assert_record(&second_capture.records[0], with_time, sizeof(with_time));
	rfid_sample_protocol_feed(&first, without_time, sizeof(without_time), 101U,
			   capture_record, &first_capture);
	assert(first_capture.count == 1U);
	assert(get_stats(&second).frames == 1U);
}

static void test_reset_preserves_stats_and_timeout(void)
{
	struct rfid_sample_protocol protocol;
	struct capture capture = {0};
	struct rfid_sample_protocol_stats before;
	struct rfid_sample_protocol_stats after;

	rfid_sample_protocol_init(&protocol, 200U);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 0U,
			   capture_record, &capture);
	rfid_sample_protocol_feed(&protocol, with_time, 8U, 1U, capture_record, &capture);
	before = get_stats(&protocol);
	rfid_sample_protocol_reset(&protocol);
	after = get_stats(&protocol);
	assert_stats_equal(&before, &after);
	rfid_sample_protocol_feed(&protocol, with_time + 8U, sizeof(with_time) - 8U,
			   2U, capture_record, &capture);
	assert(capture.count == 1U);
	rfid_sample_protocol_feed(&protocol, without_time, 8U, 1000U,
			   capture_record, &capture);
	rfid_sample_protocol_expire(&protocol, 1199U);
	assert(get_stats(&protocol).partial_timeouts == 0U);
	rfid_sample_protocol_expire(&protocol, 1200U);
	assert(get_stats(&protocol).partial_timeouts == 1U);
	assert(get_stats(&protocol).frames == 1U);
}

static void test_null_callback_and_null_arguments(void)
{
	struct rfid_sample_protocol protocol;
	struct rfid_sample_protocol_stats zero = {0};
	struct rfid_sample_protocol_stats stats;

	rfid_sample_protocol_init(NULL, 0U);
	rfid_sample_protocol_reset(NULL);
	rfid_sample_protocol_expire(NULL, 0U);
	rfid_sample_protocol_feed(NULL, without_time, sizeof(without_time), 0U, NULL, NULL);
	rfid_sample_protocol_get_stats(NULL, NULL);
	rfid_sample_protocol_get_stats(NULL, &stats);
	assert_stats_equal(&stats, &zero);
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, NULL, 1U, 0U, NULL, NULL);
	stats = get_stats(&protocol);
	assert_stats_equal(&stats, &zero);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 0U, NULL, NULL);
	assert(get_stats(&protocol).frames == 1U);
	rfid_sample_protocol_get_stats(&protocol, NULL);
	rfid_sample_protocol_init(&protocol, 0U);
	stats = get_stats(&protocol);
	assert_stats_equal(&stats, &zero);
}

static void test_mode_responses(void)
{
	struct rfid_sample_protocol protocol;
	struct capture tags = {0};
	struct response_capture responses = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
	for (uint8_t mode = 0U; mode <= 2U; mode++) {
		uint8_t frame[] = {0x1b, 0x22, 0x7f, 0x42, 0x02, 0x00,
				   0x08, mode, (uint8_t)(0x08U ^ mode)};
		const struct rfid_sample_response *response;

		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), mode,
					  capture_record, &tags);
		assert(responses.count == (size_t)mode + 1U);
		response = &responses.responses[mode];
		assert(response->command == 0x22U);
		assert(response->sequence == 0x7fU);
		assert(response->address == 0x42U);
		assert(response->device_error == 0U);
		assert(response->has_mode);
		assert(response->mode == mode);
	}
	for (uint8_t error = 0x80U; error <= 0x82U; error++) {
		uint8_t frame[] = {0x1b, 0x22, 0xff, 0x01, 0x01, 0x00, error, error};
		const struct rfid_sample_response *response;
		size_t output = 3U + (size_t)(error - 0x80U);

		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), 4U,
					  capture_record, &tags);
		assert(responses.count == output + 1U);
		response = &responses.responses[output];
		assert(response->command == 0x22U);
		assert(response->sequence == 0xffU);
		assert(response->address == 0x01U);
		assert(response->device_error == error);
		assert(!response->has_mode);
	}
	assert(tags.count == 0U);
	assert(get_stats(&protocol).frames == 0U);
	assert(get_stats(&protocol).checksum_errors == 0U);
	assert(get_stats(&protocol).sync_losses == 0U);
}

static void test_invalid_mode_responses(void)
{
	static const uint8_t invalid[][9] = {
		{0x1b, 0x22, 0x00, 0xff, 0x02, 0x00, 0x08, 0x03, 0x0b},
		{0x1b, 0x22, 0x00, 0xff, 0x02, 0x00, 0x07, 0x01, 0x06},
		{0x1b, 0x22, 0x00, 0xff, 0x01, 0x00, 0x00, 0x00, 0x00},
		{0x1b, 0x22, 0x00, 0xff, 0x01, 0x00, 0x83, 0x83, 0x00},
	};

	for (size_t i = 0U; i < ARRAY_SIZE(invalid); i++) {
		struct rfid_sample_protocol protocol;
		struct capture tags = {0};
		struct response_capture responses = {0};
		size_t size = (size_t)invalid[i][4] + 7U;

		rfid_sample_protocol_init(&protocol, 0U);
		rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
		rfid_sample_protocol_feed(&protocol, invalid[i], size, 0U, capture_record, &tags);
		rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 1U,
					  capture_record, &tags);
		assert(responses.count == 0U);
		assert(tags.count == 1U);
		assert(get_stats(&protocol).unsupported_messages == 1U);
		assert(get_stats(&protocol).checksum_errors == 0U);
		assert(get_stats(&protocol).noise_bytes == size);
	}
}

static void test_mode_checksum_and_lengths(void)
{
	static const uint8_t bad_checksum[] = {
		0x1b, 0x22, 0x00, 0xff, 0x02, 0x00, 0x08, 0x02, 0xff,
	};
	static const uint8_t valid[] = {
		0x1b, 0x22, 0x00, 0xff, 0x02, 0x00, 0x08, 0x02, 0x0a,
	};
	static const uint16_t invalid_lengths[] = {0U, 3U, 10U, 18U, 0xffffU};
	struct rfid_sample_protocol protocol;
	struct response_capture responses = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
	rfid_sample_protocol_feed(&protocol, bad_checksum, sizeof(bad_checksum), 0U, NULL, NULL);
	assert(responses.count == 0U);
	assert(get_stats(&protocol).checksum_errors == 1U);
	assert(get_stats(&protocol).noise_bytes == sizeof(bad_checksum));
	rfid_sample_protocol_feed(&protocol, valid, sizeof(valid), 1U, NULL, NULL);
	assert(responses.count == 1U);

	for (size_t i = 0U; i < ARRAY_SIZE(invalid_lengths); i++) {
		uint8_t frame[sizeof(valid)];

		memcpy(frame, valid, sizeof(frame));
		frame[4] = (uint8_t)(invalid_lengths[i] & 0xffU);
		frame[5] = (uint8_t)(invalid_lengths[i] >> 8);
		rfid_sample_protocol_feed(&protocol, frame, sizeof(frame), 2U, NULL, NULL);
		assert(responses.count == 1U);
		assert(get_stats(&protocol).length_errors == i + 1U);
	}
}

static void test_mixed_response_and_tag_fragments(void)
{
	static const uint8_t mode[] = {
		0x1b, 0x22, 0xa5, 0xff, 0x02, 0x00, 0x08, 0x01, 0x09,
	};
	uint8_t stream[sizeof(without_time) + sizeof(mode) + sizeof(with_time)];

	memcpy(stream, without_time, sizeof(without_time));
	memcpy(stream + sizeof(without_time), mode, sizeof(mode));
	memcpy(stream + sizeof(without_time) + sizeof(mode), with_time, sizeof(with_time));
	for (size_t split = 1U; split < sizeof(stream); split++) {
		struct rfid_sample_protocol protocol;
		struct capture tags = {0};
		struct response_capture responses = {0};

		rfid_sample_protocol_init(&protocol, 0U);
		rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
		rfid_sample_protocol_feed(&protocol, stream, split, 0U, capture_record, &tags);
		rfid_sample_protocol_feed(&protocol, stream + split, sizeof(stream) - split,
					  1U, capture_record, &tags);
		assert(tags.count == 2U);
		assert_record(&tags.records[0], without_time, sizeof(without_time));
		assert_record(&tags.records[1], with_time, sizeof(with_time));
		assert(responses.count == 1U);
		assert(responses.responses[0].sequence == 0xa5U);
		assert(responses.responses[0].has_mode);
		assert(responses.responses[0].mode == 1U);
		assert(get_stats(&protocol).frames == 2U);
		assert(get_stats(&protocol).noise_bytes == 0U);
	}
}

static void test_response_callback_lifecycle(void)
{
	static const uint8_t mode[] = {
		0x1b, 0x22, 0x00, 0xff, 0x02, 0x00, 0x08, 0x00, 0x08,
	};
	struct rfid_sample_protocol protocol;
	struct response_capture responses = {0};

	rfid_sample_protocol_set_response_callback(NULL, capture_response, &responses);
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, mode, sizeof(mode), 0U, NULL, NULL);
	assert(get_stats(&protocol).noise_bytes == 0U);
	rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
	rfid_sample_protocol_feed(&protocol, mode, 5U, 1U, NULL, NULL);
	rfid_sample_protocol_reset(&protocol);
	rfid_sample_protocol_feed(&protocol, mode, sizeof(mode), 2U, NULL, NULL);
	assert(responses.count == 1U);
	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_feed(&protocol, mode, sizeof(mode), 3U, NULL, NULL);
	assert(responses.count == 1U);
	rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
	rfid_sample_protocol_set_response_callback(&protocol, NULL, NULL);
	rfid_sample_protocol_feed(&protocol, mode, sizeof(mode), 4U, NULL, NULL);
	assert(responses.count == 1U);
}

static void test_no_undefined_read_acknowledgements(void)
{
	static const uint8_t candidates[][8] = {
		{0x1b, 0x39, 0x01, 0xff, 0x01, 0x00, 0x00, 0x00},
		{0x1b, 0x3b, 0x01, 0xff, 0x01, 0x00, 0x80, 0x80},
	};
	struct rfid_sample_protocol protocol;
	struct capture tags = {0};
	struct response_capture responses = {0};

	rfid_sample_protocol_init(&protocol, 0U);
	rfid_sample_protocol_set_response_callback(&protocol, capture_response, &responses);
	for (size_t i = 0U; i < ARRAY_SIZE(candidates); i++) {
		rfid_sample_protocol_feed(&protocol, candidates[i], sizeof(candidates[i]),
					  0U, capture_record, &tags);
	}
	assert(tags.count == 0U);
	assert(responses.count == 0U);
	assert(get_stats(&protocol).frames == 0U);
	rfid_sample_protocol_feed(&protocol, without_time, sizeof(without_time), 1U,
				  capture_record, &tags);
	assert(tags.count == 1U);
}

static void test_request_encoding(void)
{
	static const struct {
		uint8_t command;
		uint8_t address;
		size_t size;
		uint8_t frame[RFID_SAMPLE_PROTOCOL_MAX_REQUEST_SIZE];
	} requests[] = {
		{0x39U, 0xffU, 7U, {0x1b, 0x39, 0x01, 0xff, 0x00, 0x00, 0x00}},
		{0x3bU, 0x42U, 7U, {0x1b, 0x3b, 0x01, 0x42, 0x00, 0x00, 0x00}},
		{0x22U, 0x00U, 8U, {0x1b, 0x22, 0x00, 0x00, 0x01, 0x00, 0x08, 0x08}},
	};
	uint8_t output[RFID_SAMPLE_PROTOCOL_MAX_REQUEST_SIZE + 1U];

	for (size_t i = 0U; i < ARRAY_SIZE(requests); i++) {
		memset(output, 0xa5, sizeof(output));
		assert(rfid_sample_protocol_make_request(requests[i].command,
				requests[i].address, output, requests[i].size) == requests[i].size);
		assert(memcmp(output, requests[i].frame, requests[i].size) == 0);
		assert(output[requests[i].size] == 0xa5U);
		for (size_t capacity = 0U; capacity < requests[i].size; capacity++) {
			memset(output, 0xa5, sizeof(output));
			assert(rfid_sample_protocol_make_request(requests[i].command,
					requests[i].address, output, capacity) == 0U);
			for (size_t byte = 0U; byte < sizeof(output); byte++) {
				assert(output[byte] == 0xa5U);
			}
		}
	}
	assert(rfid_sample_protocol_make_request(0x39U, 0xffU, NULL, sizeof(output)) == 0U);
	memset(output, 0xa5, sizeof(output));
	assert(rfid_sample_protocol_make_request(0x27U, 0xffU, output, sizeof(output)) == 0U);
	assert(rfid_sample_protocol_make_request(0x01U, 0xffU, output, sizeof(output)) == 0U);
	for (size_t byte = 0U; byte < sizeof(output); byte++) {
		assert(output[byte] == 0xa5U);
	}
}

int main(void)
{
	test_complete_frames();
	test_every_fragment_boundary(without_time, sizeof(without_time));
	test_every_fragment_boundary(with_time, sizeof(with_time));
	test_bytewise(without_time, sizeof(without_time));
	test_bytewise(with_time, sizeof(with_time));
	test_concatenated_frames();
	test_noise_and_overlapping_sync();
	test_wrong_message_type();
	test_invalid_packet_lengths();
	test_invalid_tag_lengths();
	test_tag_length_rejection_preserves_next_header();
	test_checksum_is_not_verified();
	test_raw_fields_and_big_endian_uid();
	test_invalid_time_is_retained();
	test_timeout_boundary_and_recovery();
	test_total_age_and_zero_length_poll();
	test_resync_preserves_original_arrival_age();
	test_backwards_clock();
	test_default_timeout();
	test_instance_isolation();
	test_reset_preserves_stats_and_timeout();
	test_null_callback_and_null_arguments();
	test_mode_responses();
	test_invalid_mode_responses();
	test_mode_checksum_and_lengths();
	test_mixed_response_and_tag_fragments();
	test_response_callback_lifecycle();
	test_no_undefined_read_acknowledgements();
	test_request_encoding();
	puts("RFID protocol diagnostic parser tests passed (checksum not verified)");
	return 0;
}
