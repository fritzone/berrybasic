/*
 *  The BerryBasiC seed-packet (.PKT) format: a static library for PODs.
 *
 *  A packet is an index followed by concatenated compiled objects. The index
 *  says which symbols each member defines, so the linker can pull only the
 *  members it needs to satisfy a POD's undefined references, unioning each
 *  pulled member's declared capabilities into the POD. It never appears at run
 *  time: .PKT is purely a build-time concern.  See doc "Seed Packets".
 *
 *  Layout:  Header | member table | symbol table | name pool | member data
 *  All fields little-endian.
 */
#ifndef PKT_H
#define PKT_H

#define PKT_MAGIC0 0x50  /* 'P' */
#define PKT_MAGIC1 0x4B  /* 'K' */
#define PKT_MAGIC2 0x54  /* 'T' */
#define PKT_MAGIC3 0x21  /* '!' */
#define PKT_FORMAT_VERSION 1

/* Header, 32 bytes. */
#define PKT_H_MAGIC        0   /* 8: "PKT!\r\n\x1A\n"                     */
#define PKT_H_FORMAT_VER   8   /* u16                                    */
#define PKT_H_FLAGS       10   /* u16 (reserved, 0)                      */
#define PKT_H_MEMBER_CNT  12   /* u32                                    */
#define PKT_H_SYMBOL_CNT  16   /* u32                                    */
#define PKT_H_NAMEPOOL    20   /* u32: file offset of the name pool      */
#define PKT_H_FILESIZE    24   /* u32                                    */
#define PKT_H_HEADER_CRC  28   /* u32: CRC-32C over bytes 0..27          */
#define PKT_HDR_SIZE      32

/* Member record, 32 bytes. */
#define PKT_M_NAME_OFF     0   /* u32: offset into the name pool         */
#define PKT_M_DATA_OFF     4   /* u32: file offset of the object bytes   */
#define PKT_M_DATA_SIZE    8   /* u32                                    */
#define PKT_M_CRC         12   /* u32: CRC-32C over the member's bytes   */
#define PKT_M_CAPS        16   /* u64: capabilities this member needs    */
#define PKT_M_RESERVED    24   /* u64: 0                                 */
#define PKT_MEMBER_SIZE   32

/* Symbol record, 20 bytes. Sorted by name; only definitions are listed. */
#define PKT_S_NAME_OFF     0   /* u32: offset into the name pool         */
#define PKT_S_MEMBER       4   /* u32: index of the defining member      */
#define PKT_S_KIND         8   /* u8: 0 = function, 1 = data             */
#define PKT_S_RESERVED     9   /* 11 bytes: 0                            */
#define PKT_SYMBOL_SIZE   20

#define PKT_SYM_FUNC 0
#define PKT_SYM_DATA 1

#endif /* PKT_H */
