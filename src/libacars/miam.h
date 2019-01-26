/*
 *  This file is a part of libacars
 *
 *  Copyright (c) 2018-2019 Tomasz Lemiech <szpajder@gmail.com>
 */

#ifndef LA_MIAM_H
#define LA_MIAM_H 1

#include <stdint.h>
#include <libacars/libacars.h>		// la_type_descriptor, la_proto_node
#include <libacars/vstring.h>		// la_vstring

#ifdef __cplusplus
extern "C" {
#endif

// MIAM frame identifier
typedef enum {
	LA_MIAM_FID_UNKNOWN = 0,
	LA_MIAM_FID_SINGLE_TRANSFER,
	LA_MIAM_FID_FILE_TRANSFER_REQ,
	LA_MIAM_FID_FILE_TRANSFER_ACCEPT,
	LA_MIAM_FID_FILE_SEGMENT,
	LA_MIAM_FID_TRANSFER_ABORT,
	LA_MIAM_FID_XOFF_IND,
	LA_MIAM_FID_XON_IND
} la_miam_frame_id;
#define LA_MIAM_FRAME_ID_CNT 8

// MIAM ACARS CF frame
typedef struct {
	la_miam_frame_id frame_id;
} la_miam_msg;

la_proto_node *la_miam_parse(char const * const label, char const *txt, la_msg_dir const msg_dir);
la_proto_node *la_miam_single_transfer_parse(char const * const label, char const *txt, la_msg_dir const msg_dir);

void la_miam_format_text(la_vstring * const vstr, void const * const data, int indent);
void la_miam_single_transfer_format_text(la_vstring * const vstr, void const * const data, int indent);

extern la_type_descriptor const la_DEF_miam_message;
extern la_type_descriptor const la_DEF_single_transfer_message;
la_proto_node *la_proto_tree_find_miam(la_proto_node *root);

#ifdef __cplusplus
}
#endif

#endif // !LA_MIAM_H
