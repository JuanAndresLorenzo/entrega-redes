/**********************************************************************
 * file:  sr_router.c
 *
 * Descripción:
 *
 * Este archivo contiene todas las funciones que interactúan directamente
 * con la tabla de enrutamiento, así como el método de entrada principal
 * para el enrutamiento.
 *
 **********************************************************************/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"

/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Inicializa el subsistema de enrutamiento
 *
 *---------------------------------------------------------------------*/

void sr_init(struct sr_instance *sr)
{
  assert(sr);

  /* Inicializa la caché y el hilo de limpieza de la caché */
  sr_arpcache_init(&(sr->cache));

  /* Inicializa los atributos del hilo */
  pthread_attr_init(&(sr->attr));
  pthread_attr_setdetachstate(&(sr->attr), PTHREAD_CREATE_JOINABLE);
  pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
  pthread_attr_setscope(&(sr->attr), PTHREAD_SCOPE_SYSTEM);
  pthread_t thread;

  /* Hilo para gestionar el timeout del caché ARP */
  pthread_create(&thread, &(sr->attr), sr_arpcache_timeout, sr);

} /* -- sr_init -- */

void sr_forward_packet(struct sr_instance *sr,
                       uint8_t *packet /* lent */,
                       unsigned int len,
                       uint8_t *mac_dest,
                       char *interface /* lent */)
{

  /* Obtener la cabecera Ethernet y modificar la dirección MAC de destino */
  sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)packet;

  /* Obtener la interfaz de salida */
  struct sr_if *out_iface = sr_get_interface(sr, interface);
  if (out_iface == NULL)
  {
    fprintf(stderr, "Error: la interfaz de salida %s no se encontró.\n", interface);
    return;
  }

  /* Configurar la dirección MAC de origen a la MAC de la interfaz de salida */
  memcpy(ethernet_hdr->ether_shost, out_iface->addr, ETHER_ADDR_LEN);

  /* Configurar la dirección MAC de destino */
  memcpy(ethernet_hdr->ether_dhost, mac_dest, ETHER_ADDR_LEN);

  /* Enviar el paquete por la interfaz indicada */
  sr_send_packet(sr, packet, len, interface);

  fprintf(stdout, "Paquete reenviado a la interfaz %s\n", interface);
}

/* Function to count the number of set bits (1s) in the mask */
int count_set_bits(uint32_t mask)
{
  int count = 0;
  while (mask)
  {
    count += mask & 1;
    mask >>= 1;
  }
  return count;
}

struct sr_rt *sr_find_rt_entry(struct sr_instance *sr, uint32_t ip)
{
  /* Find the entry with the longest matching prefix */
  struct sr_rt *best_match = NULL;
  uint32_t longest_match_len = 0;

  struct sr_rt *rt_iter = sr->routing_table;

  while (rt_iter != NULL)
  {
    /* Compare the destination IP address with the entry in the routing table using the mask */
    if ((rt_iter->mask.s_addr & ip) == (rt_iter->dest.s_addr & rt_iter->mask.s_addr))
    {
      /* Calculate the prefix length (number of set bits in the mask) */
      uint32_t mask_len = count_set_bits(ntohl(rt_iter->mask.s_addr));

      /* Find the entry with the longest matching prefix */
      if (mask_len > longest_match_len)
      {
        best_match = rt_iter;
        longest_match_len = mask_len;
      }
    }
    rt_iter = rt_iter->next;
  }

  return best_match;
}

/* Envía un paquete ICMP de error */
void sr_send_icmp_error_packet(uint8_t type,
                               uint8_t code,
                               struct sr_instance *sr,
                               uint32_t ipDst,
                               uint8_t *ipPacket)
{
  /* Crear un nuevo paquete ICMP */
  unsigned int icmp_len = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
  uint8_t *icmp_packet = malloc(icmp_len);

  /* Obtener las cabeceras del paquete original */
  sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)ipPacket;
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(ipPacket + sizeof(sr_ethernet_hdr_t));

  /* Buscar la mejor coincidencia en la tabla de enrutamiento */
  struct sr_rt *rt_match = sr_find_rt_entry(sr, ip_hdr->ip_src);
  if (!rt_match)
  {
    fprintf(stderr, "Error: no se encontró una ruta para el destino %u\n", ip_hdr->ip_src);
    free(icmp_packet);
    return;
  }

  /* Obtener la interfaz de salida a partir de la entrada de la tabla de enrutamiento */
  struct sr_if *out_iface = sr_get_interface(sr, rt_match->interface);
  if (!out_iface)
  {
    fprintf(stderr, "Error: no se pudo encontrar la interfaz de salida\n");
    free(icmp_packet);
    return;
  }

  /* Llenar la nueva cabecera Ethernet */
  sr_ethernet_hdr_t *new_ethernet_hdr = (sr_ethernet_hdr_t *)icmp_packet;
  memcpy(new_ethernet_hdr->ether_shost, out_iface->addr, ETHER_ADDR_LEN); /* MAC de origen (la interfaz de salida) */

  /* Buscar la dirección MAC del próximo salto (mediante ARP) */
  struct sr_arpentry *arp_entry = sr_arpcache_lookup(&(sr->cache), rt_match->gw.s_addr);
  if (arp_entry)
  {
    memcpy(new_ethernet_hdr->ether_dhost, arp_entry->mac, ETHER_ADDR_LEN); /* MAC de destino (próximo salto) */
    free(arp_entry);
  }
  else
  {
    /* Si no existe una entrada ARP, enviar una solicitud ARP */
    struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), rt_match->gw.s_addr, icmp_packet, icmp_len, out_iface->name);
    handle_arpreq(sr, req);
    return; /* Salir, ya que el paquete ICMP se enviará cuando se reciba la respuesta ARP */
  }

  new_ethernet_hdr->ether_type = htons(ethertype_ip); /* Tipo de paquete Ethernet (IP) */

  /* Llenar la nueva cabecera IP */
  sr_ip_hdr_t *new_ip_hdr = (sr_ip_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t));
  new_ip_hdr->ip_v = 4;                                             /* Versión IP */
  new_ip_hdr->ip_hl = sizeof(sr_ip_hdr_t) / 4;                      /* Longitud de cabecera */
  new_ip_hdr->ip_tos = 0;                                           /* Tipo de servicio */
  new_ip_hdr->ip_len = htons(icmp_len - sizeof(sr_ethernet_hdr_t)); /* Longitud total del paquete IP */
  new_ip_hdr->ip_id = htons(0);                                     /* ID del paquete */
  new_ip_hdr->ip_off = htons(IP_DF);                                /* No fragmentar */
  new_ip_hdr->ip_ttl = 64;                                          /* TTL */
  new_ip_hdr->ip_p = ip_protocol_icmp;                              /* Protocolo ICMP */
  new_ip_hdr->ip_src = out_iface->ip;                               /* IP de origen del router (interfaz de salida) */
  new_ip_hdr->ip_dst = ip_hdr->ip_src;                              /* IP de destino (origen del paquete original) */

  /* Recalcular el checksum de la cabecera IP */
  new_ip_hdr->ip_sum = 0;
  new_ip_hdr->ip_sum = cksum(new_ip_hdr, sizeof(sr_ip_hdr_t));

  /* Llenar la cabecera ICMP */
  sr_icmp_t3_hdr_t *icmp_hdr = (sr_icmp_t3_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
  icmp_hdr->icmp_type = type; /* Tipo de mensaje ICMP (ej. 3: Destination Unreachable, 11: Time Exceeded) */
  icmp_hdr->icmp_code = code; /* Código del mensaje ICMP (ej. 0: Network Unreachable) */
  icmp_hdr->icmp_sum = 0;

  /* Copiar los primeros 8 bytes del paquete original en el campo de datos del mensaje ICMP */
  memcpy(icmp_hdr->data, ip_hdr, sizeof(sr_ip_hdr_t) + 8);

  /* Recalcular el checksum de la cabecera ICMP */
  icmp_hdr->icmp_sum = cksum(icmp_hdr, sizeof(sr_icmp_t3_hdr_t));

  /* Enviar el paquete */
  sr_send_packet(sr, icmp_packet, icmp_len, out_iface->name);

  free(icmp_packet);

} /* -- sr_send_icmp_error_packet -- */

/* FUNCIONES AUXILIARES */

void sr_send_icmp(struct sr_instance *sr,
                  uint8_t *original_packet /* lent */,
                  unsigned int len,
                  uint8_t icmp_type,
                  uint8_t icmp_code,
                  char *interface /* lent */)
{
  /* Crear un nuevo paquete ICMP para la respuesta */
  unsigned int new_len = len;
  uint8_t *icmp_packet = malloc(new_len);
  memcpy(icmp_packet, original_packet, len);

  /* Obtener cabeceras de Ethernet e IP */
  sr_ethernet_hdr_t *ethernet_hdr = (sr_ethernet_hdr_t *)icmp_packet;
  sr_ip_hdr_t *ip_hdr = (sr_ip_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t));
  sr_icmp_hdr_t *icmp_hdr = (sr_icmp_hdr_t *)(icmp_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));

  /* Intercambiar direcciones MAC de origen y destino */
  uint8_t temp_mac[ETHER_ADDR_LEN];
  memcpy(temp_mac, ethernet_hdr->ether_shost, ETHER_ADDR_LEN);                  /* Guardar MAC origen */
  memcpy(ethernet_hdr->ether_shost, ethernet_hdr->ether_dhost, ETHER_ADDR_LEN); /* MAC destino a origen */
  memcpy(ethernet_hdr->ether_dhost, temp_mac, ETHER_ADDR_LEN);                  /* MAC origen guardada a destino */

  /* Intercambiar direcciones IP de origen y destino */
  uint32_t temp_ip = ip_hdr->ip_src;
  ip_hdr->ip_src = ip_hdr->ip_dst;
  ip_hdr->ip_dst = temp_ip;

  /* Setear el tipo y codigo a la cabecera ICMP */
  icmp_hdr->icmp_type = icmp_type;
  icmp_hdr->icmp_code = icmp_code;
  icmp_hdr->icmp_sum = 0;
  icmp_hdr->icmp_sum = cksum(icmp_hdr, new_len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));

  /* Recalcular el checksum de la cabecera IP */
  ip_hdr->ip_sum = 0;
  ip_hdr->ip_sum = cksum(ip_hdr, sizeof(sr_ip_hdr_t));

  /* Enviar el paquete ICMP */
  sr_send_packet(sr, icmp_packet, new_len, interface);

  free(icmp_packet);
}

void sr_handle_ip_packet(struct sr_instance *sr,
                         uint8_t *packet /* lent */,
                         unsigned int len,
                         uint8_t *srcAddr,
                         uint8_t *destAddr,
                         char *interface /* lent */,
                         sr_ethernet_hdr_t *eHdr)
{

  /* Verificar que el tamaño del paquete sea suficiente para la cabecera IP */
  if (len < (sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)))
  {
    fprintf(stderr, "Paquete IP demasiado corto. Descartando paquete.\n");
    return;
  }

  /* Extraer cabecera IP */
  sr_ip_hdr_t *ip_header = (sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));
  print_hdr_ip((uint8_t *)ip_header);

  /* Verificar checksum de la cabecera IP */
  uint16_t calculated_checksum = ip_cksum(ip_header, sizeof(sr_ip_hdr_t));

  if (ip_header->ip_sum != calculated_checksum)
  {
    fprintf(stderr, "Checksum IP inválido. Descartando paquete.\n");
    return;
  }

  /* Obtener las direcciones de origen y destino */
  uint32_t ip_src = ip_header->ip_src;
  uint32_t ip_dst = ip_header->ip_dst;

  fprintf(stdout, "Paquete IP recibido: \n");
  print_addr_ip_int(ntohl(ip_src));
  print_addr_ip_int(ntohl(ip_dst));

  /* Verificar si el paquete es para una de las interfaces del router */
  struct sr_if *iface_check = sr->if_list;
  int is_for_me = 0;
  while (iface_check != NULL)
  {
    if (iface_check->ip == ip_dst)
    {
      is_for_me = 1;
      break;
    }
    iface_check = iface_check->next;
  }

  /* Verificar TTL */
  if (ip_header->ip_ttl <= 1)
  {
    fprintf(stderr, "TTL expirado. Enviando ICMP Time Exceeded.\n");
    sr_send_icmp_error_packet(11, 0, sr, ip_header->ip_src, packet); /* Tipo 11, código 0: TTL Expired */
    return;
  }

  /* Verificar si el paquete es un paquete ICMP y es para mi */
  if (is_for_me)
  {
    if (ip_protocol((uint8_t *)ip_header) == ip_protocol_icmp)
    {
      sr_icmp_hdr_t *icmp_header = (sr_icmp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
      if ((icmp_header->icmp_type == 8) && (icmp_header->icmp_code == 0))
      {
        fprintf(stderr, "ICMP Echo Request recibido. Respondiendo con Echo Reply.\n");
        sr_send_icmp(sr, packet, len, 0, 0, interface); /* Tipo 0, Código 0: Echo Reply*/
      }
      return;
    }
    else
    {
      sr_send_icmp_error_packet(3, 3, sr, ip_src, packet); /*Tipo 3 (Destination unreachable), Código 3 (Port Unreachable)*/
      fprintf(stderr, "Paquete ICMP no reconocido. Descartando paquete.\n");
      return;
    }
  }

  /* Si el paquete no es para mi, busco coincidencia en la tabla de enrutamiento */
  if (!is_for_me)
  {
    struct sr_rt *rt_match = sr_find_rt_entry(sr, ip_dst);
    if (rt_match == NULL)
    {
      fprintf(stderr, "No se encontró ruta para %u. Enviando ICMP net unreachable.\n", ip_dst);
      sr_send_icmp_error_packet(3, 0, sr, ip_src, packet); /* Tipo 3, Código 0: Network Unreachable */
      return;
    }

    /* Decrementar TTL y recalcular checksum */
    ip_header->ip_ttl--;
    ip_header->ip_sum = 0;
    ip_header->ip_sum = ip_cksum(ip_header, sizeof(sr_ip_hdr_t));

    /* Obtener la dirección MAC del siguiente salto usando ARP */
    struct sr_arpentry *arp_entry = sr_arpcache_lookup(&sr->cache, rt_match->gw.s_addr);
    if (arp_entry)
    {
      /* Reenviar el paquete si la dirección MAC está disponible */
      fprintf(stdout, "Reenviando el paquete al siguiente salto.\n");
      sr_forward_packet(sr, packet, len, arp_entry->mac, rt_match->interface);
      free(arp_entry);
    }
    else
    {
      /* Solicitar ARP si no se conoce la dirección MAC */
      fprintf(stdout, "Solicitando ARP para la dirección %u.\n", ntohl(rt_match->gw.s_addr));
      struct sr_arpreq *req = sr_arpcache_queuereq(&sr->cache, rt_match->gw.s_addr, packet, len, interface);
      handle_arpreq(sr, req);
    }
  }
}

/*
 * ***** A partir de aquí no debería tener que modificar nada ****
 */

/* Envía todos los paquetes IP pendientes de una solicitud ARP */
void sr_arp_reply_send_pending_packets(struct sr_instance *sr,
                                       struct sr_arpreq *arpReq,
                                       uint8_t *dhost,
                                       uint8_t *shost,
                                       struct sr_if *iface)
{

  struct sr_packet *currPacket = arpReq->packets;
  sr_ethernet_hdr_t *ethHdr;
  uint8_t *copyPacket;

  while (currPacket != NULL)
  {
    ethHdr = (sr_ethernet_hdr_t *)currPacket->buf;
    memcpy(ethHdr->ether_shost, dhost, sizeof(uint8_t) * ETHER_ADDR_LEN);
    memcpy(ethHdr->ether_dhost, shost, sizeof(uint8_t) * ETHER_ADDR_LEN);

    copyPacket = malloc(sizeof(uint8_t) * currPacket->len);
    memcpy(copyPacket, ethHdr, sizeof(uint8_t) * currPacket->len);

    print_hdrs(copyPacket, currPacket->len);
    sr_send_packet(sr, copyPacket, currPacket->len, iface->name);
    currPacket = currPacket->next;
  }
}

/* Gestiona la llegada de un paquete ARP*/
void sr_handle_arp_packet(struct sr_instance *sr,
                          uint8_t *packet /* lent */,
                          unsigned int len,
                          uint8_t *srcAddr,
                          uint8_t *destAddr,
                          char *interface /* lent */,
                          sr_ethernet_hdr_t *eHdr)
{

  /* Imprimo el cabezal ARP */
  printf("*** -> It is an ARP packet. Print ARP header.\n");
  print_hdr_arp(packet + sizeof(sr_ethernet_hdr_t));

  /* Obtengo el cabezal ARP */
  sr_arp_hdr_t *arpHdr = (sr_arp_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t));

  /* Obtengo las direcciones MAC */
  unsigned char senderHardAddr[ETHER_ADDR_LEN], targetHardAddr[ETHER_ADDR_LEN];
  memcpy(senderHardAddr, arpHdr->ar_sha, ETHER_ADDR_LEN);
  memcpy(targetHardAddr, arpHdr->ar_tha, ETHER_ADDR_LEN);

  /* Obtengo las direcciones IP */
  uint32_t senderIP = arpHdr->ar_sip;
  uint32_t targetIP = arpHdr->ar_tip;
  unsigned short op = ntohs(arpHdr->ar_op);

  /* Verifico si el paquete ARP es para una de mis interfaces */
  struct sr_if *myInterface = sr_get_interface_given_ip(sr, targetIP);

  if (op == arp_op_request)
  { /* Si es un request ARP */
    printf("**** -> It is an ARP request.\n");

    /* Si el ARP request es para una de mis interfaces */
    if (myInterface != 0)
    {
      printf("***** -> ARP request is for one of my interfaces.\n");

      /* Agrego el mapeo MAC->IP del sender a mi caché ARP */
      printf("****** -> Add MAC->IP mapping of sender to my ARP cache.\n");
      sr_arpcache_insert(&(sr->cache), senderHardAddr, senderIP);

      /* Construyo un ARP reply y lo envío de vuelta */
      printf("****** -> Construct an ARP reply and send it back.\n");
      memcpy(eHdr->ether_shost, (uint8_t *)myInterface->addr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      memcpy(eHdr->ether_dhost, (uint8_t *)senderHardAddr, sizeof(uint8_t) * ETHER_ADDR_LEN);
      memcpy(arpHdr->ar_sha, myInterface->addr, ETHER_ADDR_LEN);
      memcpy(arpHdr->ar_tha, senderHardAddr, ETHER_ADDR_LEN);
      arpHdr->ar_sip = targetIP;
      arpHdr->ar_tip = senderIP;
      arpHdr->ar_op = htons(arp_op_reply);

      /* Imprimo el cabezal del ARP reply creado */
      print_hdrs(packet, len);

      sr_send_packet(sr, packet, len, myInterface->name);
    }

    printf("******* -> ARP request processing complete.\n");
  }
  else if (op == arp_op_reply)
  { /* Si es un reply ARP */

    printf("**** -> It is an ARP reply.\n");

    /* Agrego el mapeo MAC->IP del sender a mi caché ARP */
    printf("***** -> Add MAC->IP mapping of sender to my ARP cache.\n");
    struct sr_arpreq *arpReq = sr_arpcache_insert(&(sr->cache), senderHardAddr, senderIP);

    if (arpReq != NULL)
    { /* Si hay paquetes pendientes */

      printf("****** -> Send outstanding packets.\n");
      sr_arp_reply_send_pending_packets(sr, arpReq, (uint8_t *)myInterface->addr, (uint8_t *)senderHardAddr, myInterface);
      sr_arpreq_destroy(&(sr->cache), arpReq);
    }
    printf("******* -> ARP reply processing complete.\n");
  }
}

/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/

void sr_handlepacket(struct sr_instance *sr,
                     uint8_t *packet /* lent */,
                     unsigned int len,
                     char *interface /* lent */)
{
  assert(sr);
  assert(packet);
  assert(interface);

  printf("*** -> Received packet of length %d \n", len);

  /* Obtengo direcciones MAC origen y destino */
  sr_ethernet_hdr_t *eHdr = (sr_ethernet_hdr_t *)packet;
  uint8_t *destAddr = malloc(sizeof(uint8_t) * ETHER_ADDR_LEN);
  uint8_t *srcAddr = malloc(sizeof(uint8_t) * ETHER_ADDR_LEN);
  memcpy(destAddr, eHdr->ether_dhost, sizeof(uint8_t) * ETHER_ADDR_LEN);
  memcpy(srcAddr, eHdr->ether_shost, sizeof(uint8_t) * ETHER_ADDR_LEN);
  uint16_t pktType = ntohs(eHdr->ether_type);

  if (is_packet_valid(packet, len))
  {
    if (pktType == ethertype_arp)
    {
      sr_handle_arp_packet(sr, packet, len, srcAddr, destAddr, interface, eHdr);
    }
    else if (pktType == ethertype_ip)
    {
      sr_handle_ip_packet(sr, packet, len, srcAddr, destAddr, interface, eHdr);
    }
  }

} /* end sr_ForwardPacket */