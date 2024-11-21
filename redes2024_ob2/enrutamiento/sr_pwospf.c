/*-----------------------------------------------------------------------------
 * file: sr_pwospf.c
 *
 * Descripción:
 * Este archivo contiene las funciones necesarias para el manejo de los paquetes
 * OSPF.
 *
 *---------------------------------------------------------------------------*/

#include "sr_pwospf.h"
#include "sr_router.h"

#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <malloc.h>

#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "sr_utils.h"
#include "sr_protocol.h"
#include "pwospf_protocol.h"
#include "sr_rt.h"
#include "pwospf_neighbors.h"
#include "pwospf_topology.h"
#include "dijkstra.h"

/*pthread_t hello_thread;*/
pthread_t g_hello_packet_thread;
pthread_t g_all_lsu_thread;
pthread_t g_lsu_thread;
pthread_t g_neighbors_thread;
pthread_t g_topology_entries_thread;
pthread_t g_rx_lsu_thread;
pthread_t g_dijkstra_thread;

pthread_mutex_t g_dijkstra_mutex = PTHREAD_MUTEX_INITIALIZER;

struct in_addr g_router_id;
uint8_t g_ospf_multicast_mac[ETHER_ADDR_LEN];
struct ospfv2_neighbor *g_neighbors;
struct pwospf_topology_entry *g_topology;
uint16_t g_sequence_num;

/* -- Declaración de hilo principal de la función del subsistema pwospf --- */
static void *pwospf_run_thread(void *arg);

/*---------------------------------------------------------------------
 * Method: pwospf_init(..)
 *
 * Configura las estructuras de datos internas para el subsistema pwospf
 * y crea un nuevo hilo para el subsistema pwospf.
 *
 * Se puede asumir que las interfaces han sido creadas e inicializadas
 * en este punto.
 *---------------------------------------------------------------------*/

int pwospf_init(struct sr_instance *sr)
{
    assert(sr);

    sr->ospf_subsys = (struct pwospf_subsys *)malloc(sizeof(struct
                                                            pwospf_subsys));
    fprintf(stdout, "ARRANCADO PWOSPF\n");
    assert(sr->ospf_subsys);
    pthread_mutex_init(&(sr->ospf_subsys->lock), 0);

    g_router_id.s_addr = 0;

    /* Defino la MAC de multicast a usar para los paquetes HELLO */
    g_ospf_multicast_mac[0] = 0x01;
    g_ospf_multicast_mac[1] = 0x00;
    g_ospf_multicast_mac[2] = 0x5e;
    g_ospf_multicast_mac[3] = 0x00;
    g_ospf_multicast_mac[4] = 0x00;
    g_ospf_multicast_mac[5] = 0x05;

    g_neighbors = NULL;

    g_sequence_num = 0;

    struct in_addr zero;
    zero.s_addr = 0;
    g_neighbors = create_ospfv2_neighbor(zero);
    g_topology = create_ospfv2_topology_entry(zero, zero, zero, zero, zero, 0);

    fprintf(stdout, "g_router_id: %d\n", g_router_id.s_addr);
    /* -- start thread subsystem -- */
    if (pthread_create(&sr->ospf_subsys->thread, 0, pwospf_run_thread, sr))
    {
        perror("pthread_create");
        assert(0);
    }

    return 0; /* success */
} /* -- pwospf_init -- */

/*---------------------------------------------------------------------
 * Method: pwospf_lock
 *
 * Lock mutex associated with pwospf_subsys
 *
 *---------------------------------------------------------------------*/

void pwospf_lock(struct pwospf_subsys *subsys)
{
    if (pthread_mutex_lock(&subsys->lock))
    {
        assert(0);
    }
}

/*---------------------------------------------------------------------
 * Method: pwospf_unlock
 *
 * Unlock mutex associated with pwospf subsystem
 *
 *---------------------------------------------------------------------*/

void pwospf_unlock(struct pwospf_subsys *subsys)
{
    if (pthread_mutex_unlock(&subsys->lock))
    {
        assert(0);
    }
}

/*---------------------------------------------------------------------
 * Method: pwospf_run_thread
 *
 * Hilo principal del subsistema pwospf.
 *
 *---------------------------------------------------------------------*/

static void *pwospf_run_thread(void *arg)
{
    sleep(5);

    struct sr_instance *sr = (struct sr_instance *)arg;

    /* Set the ID of the router */
    while (g_router_id.s_addr == 0)
    {
        struct sr_if *int_temp = sr->if_list;
        while (int_temp != NULL)
        {
            if (int_temp->ip > g_router_id.s_addr)
            {
                g_router_id.s_addr = int_temp->ip;
            }

            int_temp = int_temp->next;
        }
    }
    fprintf(stdout, "\n\nPWOSPF: Selecting the highest IP address on a router as the router ID\n");
    Debug("-> PWOSPF: The router ID is [%s]\n", inet_ntoa(g_router_id));

    Debug("\nPWOSPF: Detecting the router interfaces and adding their networks to the routing table\n");
    struct sr_if *int_temp = sr->if_list;
    while (int_temp != NULL)
    {
        struct in_addr ip;
        ip.s_addr = int_temp->ip;
        struct in_addr gw;
        gw.s_addr = 0x00000000;
        struct in_addr mask;
        mask.s_addr = int_temp->mask;
        struct in_addr network;
        network.s_addr = ip.s_addr & mask.s_addr;

        if (check_route(sr, network) == 0)
        {
            Debug("-> PWOSPF: Adding the directly connected network [%s, ", inet_ntoa(network));
            Debug("%s] to the routing table\n", inet_ntoa(mask));
            sr_add_rt_entry(sr, network, gw, mask, int_temp->name, 1);
        }
        int_temp = int_temp->next;
    }

    Debug("\n-> PWOSPF: Printing the forwarding table\n");
    sr_print_routing_table(sr);

    pthread_create(&g_hello_packet_thread, NULL, send_hellos, sr);
    pthread_create(&g_all_lsu_thread, NULL, send_all_lsu, sr);
    pthread_create(&g_neighbors_thread, NULL, check_neighbors_life, sr);
    pthread_create(&g_topology_entries_thread, NULL, check_topology_entries_age, sr);

    return NULL;
} /* -- run_ospf_thread -- */

/***********************************************************************************
 * Métodos para el manejo de los paquetes HELLO y LSU
 * SU CÓDIGO DEBERÍA IR AQUÍ
 * *********************************************************************************/

/*---------------------------------------------------------------------
 * Method: check_neighbors_life
 *
 * Chequea si los vecinos están vivos
 *
 *---------------------------------------------------------------------*/

void *check_neighbors_life(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;
    /*
    Cada 1 segundo, chequea la lista de vecinos.
    Si hay un cambio, se debe ajustar el neighbor id en la interfaz.
    */
   while (1)
    {
        usleep(1000000);
        pwospf_lock(sr->ospf_subsys);

        struct ospfv2_neighbor *removed_neighbors = check_neighbors_alive(g_neighbors);

        /* Procesar vecinos eliminados (actualizar la interfaz si es necesario) */
        while (removed_neighbors != NULL)
        {
            Debug("PWOSPF: Neighbor [ID = %s] removed from the interface\n", inet_ntoa(removed_neighbors->neighbor_id));

            /* Recorrer las interfaces para actualizar el ID del vecino eliminado */
            struct sr_if *iface = sr->if_list;
            while (iface != NULL)
            {
                if (iface->neighbor_id == removed_neighbors->neighbor_id.s_addr)
                {
                    Debug("PWOSPF: Clearing neighbor ID on interface %s\n", iface->name);
                    iface->neighbor_id = 0; /* Resetear el ID del vecino */
                }
                iface = iface->next;
            }

            struct ospfv2_neighbor *temp = removed_neighbors;
            removed_neighbors = removed_neighbors->next;
            free(temp); /* Liberar la memoria de los vecinos eliminados */
        }
        pwospf_unlock(sr->ospf_subsys);
    }

    return NULL;
} /* -- check_neighbors_life -- */

/*---------------------------------------------------------------------
 * Method: check_topology_entries_age
 *
 * Check if the topology entries are alive
 * and if they are not, remove them from the topology table
 *
 *---------------------------------------------------------------------*/

void *check_topology_entries_age(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    /*
    Cada 1 segundo, chequea el tiempo de vida de cada entrada
    de la topologia.
    Si hay un cambio en la topología, se llama a la función de Dijkstra
    en un nuevo hilo.
    Se sugiere también imprimir la topología resultado del chequeo.
    */

    while (1)
    {
        usleep(1000000);

        pwospf_lock(sr->ospf_subsys);

        uint8_t topology_changed = check_topology_age(g_topology);

        if (topology_changed)
        {
            Debug("PWOSPF: Topology table changed. Recomputing shortest paths.\n");

            /* Ejecuto Dijkstra en un nuevo hilo (run_dijkstra) */
            dijkstra_param_t *dijkstra_data = ((dijkstra_param_t*)(malloc(sizeof(dijkstra_param_t))));
            
            dijkstra_data->sr = sr;
            dijkstra_data->topology = g_topology;
            dijkstra_data->rid = g_router_id;
            dijkstra_data->mutex = g_dijkstra_mutex;
            sr_print_routing_table(sr);
            pthread_create(&g_dijkstra_thread, NULL, run_dijkstra, dijkstra_data);

            Debug("PWOSPF: Updated topology table:\n");
            print_topolgy_table(g_topology); /* Mostrar la tabla de topología actualizada */
            Debug("Dijkstra thread created.\n");
            sr_print_routing_table(sr);
        }

        pwospf_unlock(sr->ospf_subsys); 
    }

    return NULL;
} /* -- check_topology_entries_age -- */

/*---------------------------------------------------------------------
 * Method: send_hellos
 *
 * Para cada interfaz y cada helloint segundos, construye mensaje
 * HELLO y crea un hilo con la función para enviar el mensaje.
 *
 *---------------------------------------------------------------------*/

void *send_hellos(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    /* While true */
    while (1)
    {
        /* Se ejecuta cada 1 segundo */
        usleep(1000000);

        /* Bloqueo para evitar mezclar el envío de HELLOs y LSUs */
        pwospf_lock(sr->ospf_subsys);

        /* Chequeo todas las interfaces para enviar el paquete HELLO */
        struct sr_if *interface = sr->if_list;

        /*  Recorro las interfaces */
        while (interface != NULL)
        {
            /* Cada interfaz mantiene un contador en segundos para los HELLO */
            if (interface->helloint > 0)
            {
                interface->helloint--;
            }
            else /* helloint llega a cero entonces hay que enviar paquete HELLO */
            {
                powspf_hello_lsu_param_t *hello_data = ((powspf_hello_lsu_param_t *)(malloc(sizeof(powspf_hello_lsu_param_t))));
                hello_data->sr = sr;
                hello_data->interface = interface;
                Debug("\n\nPWOSPF: Sending HELLO packet for interface %s: \n", interface->name);
                pthread_create(&g_hello_packet_thread, NULL, send_hello_packet, hello_data);

                /* Reiniciar el contador de segundos para HELLO */
                interface->helloint = OSPF_DEFAULT_HELLOINT;
            }
            interface = interface->next;
        }

        /* Desbloqueo */
        pwospf_unlock(sr->ospf_subsys);
    };

    return NULL;
} /* -- send_hellos -- */

/*---------------------------------------------------------------------
 * Method: send_hello_packet
 *
 * Recibe un mensaje HELLO, agrega cabezales y lo envía por la interfaz
 * correspondiente.
 *
 *---------------------------------------------------------------------*/

void *send_hello_packet(void *arg)
{

   /*  Debug("*********************** ENTRE AL SEND HELLO PACKET ***********************************\n"); */
    powspf_hello_lsu_param_t *hello_param = ((powspf_hello_lsu_param_t *)(arg));

    Debug("\n\nPWOSPF: Constructing HELLO packet for interface %s: \n", hello_param->interface->name);

    sr_ethernet_hdr_t *ethernet_header = ((sr_ethernet_hdr_t *)(malloc(sizeof(sr_ethernet_hdr_t))));
    sr_ip_hdr_t *ip_header = ((sr_ip_hdr_t *)(malloc(sizeof(sr_ip_hdr_t))));
    ospfv2_hdr_t *ospf_header = ((ospfv2_hdr_t *)(malloc(sizeof(ospfv2_hdr_t))));
    ospfv2_hello_hdr_t *ospf_hello_header = ((ospfv2_hello_hdr_t *)(malloc(sizeof(ospfv2_hello_hdr_t))));

    int i;
    /* Seteo la dirección MAC de multicast para la trama a enviar */
    for (i = 0; i < ETHER_ADDR_LEN; i++)
    {
        ethernet_header->ether_dhost[i] = g_ospf_multicast_mac[i];
    }

    /* Seteo la dirección MAC origen con la dirección de mi interfaz de salida */
    for (i = 0; i < ETHER_ADDR_LEN; i++)
    {
        ethernet_header->ether_shost[i] = ((uint8_t)(hello_param->interface->addr[i]));
    }

    /* Seteo el ether_type en el cabezal Ethernet */
    ethernet_header->ether_type = htons(ethertype_ip); /* Hay que ver si esto funciona asi */

    /* Inicializo cabezal IP */
    ip_header->ip_v = 4;                                                                                /* Versión IPv4 */
    ip_header->ip_hl = 5;                                                                               /* Longitud del encabezado IP */
    ip_header->ip_tos = 0;                                                                              /* Tipo de servicio */
    ip_header->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t)); /* Longitud total */
    ip_header->ip_id = rand();         /* Identificación única */
    ip_header->ip_off = htons(0x4000); /* Fragmento */
    ip_header->ip_ttl = 64;            /* Tiempo de vida (TTL) */

    /* Seteo el protocolo en el cabezal IP para ser el de OSPF (89) */
    ip_header->ip_p = 89;

    /* Seteo IP origen con la IP de mi interfaz de salida */
    ip_header->ip_src = hello_param->interface->ip;

    /* Seteo IP destino con la IP de Multicast dada: OSPF_AllSPFRouters  */
    ip_header->ip_dst = htonl(OSPF_AllSPFRouters);

    /* Calculo y seteo el chechsum IP*/
    ip_header->ip_sum = 0;
    ip_header->ip_sum = ip_cksum(ip_header, sizeof(sr_ip_hdr_t));

    /* Inicializo cabezal de PWOSPF con version 2 y tipo HELLO */
    ospf_header->version = OSPF_V2;      /* Versión de OSPFv2 */
    ospf_header->type = OSPF_TYPE_HELLO; /* Tipo HELLO */

    /* Seteo el Router ID con mi ID */
    ospf_header->rid = g_router_id.s_addr;

    /* Seteo el Area ID en 0 */
    ospf_header->aid = htonl(0);

    /* Seteo el Authentication Type y Authentication Data en 0 */
    ospf_header->autype = 0; /* Tipo de autenticación */
    ospf_header->audata = 0; /* Datos de autenticación */

    /* Seteo máscara con la máscara de mi interfaz de salida */
    ospf_hello_header->nmask = htonl(0xffffff00);

    /* Seteo Hello Interval con OSPF_DEFAULT_HELLOINT */
    ospf_hello_header->helloint = htons(OSPF_DEFAULT_HELLOINT);

    /* Seteo Padding en 0*/
    ospf_hello_header->padding = 0;

    /* Creo el paquete a transmitir */
    int packet_length = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t);
    uint8_t *send_packet = ((uint8_t *)(malloc(packet_length)));

    /* Se agregan las cabeceras Ethernet, IP, OSPF y Hello en el orden adecuado. */
    memcpy(send_packet, ethernet_header, sizeof(sr_ethernet_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t), ip_header, sizeof(sr_ip_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t), ospf_header, sizeof(ospfv2_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t), ospf_hello_header, sizeof(ospfv2_hello_hdr_t));

    /* Calculo y actualizo el checksum del cabezal OSPF */
    ((ospfv2_hdr_t *)(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)))->csum = ospfv2_cksum((ospfv2_hdr_t *)(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)), sizeof(ospfv2_hdr_t) + sizeof(ospfv2_hello_hdr_t));

    /* Envío el paquete HELLO */
    sr_send_packet(hello_param->sr, ((uint8_t *)(send_packet)), packet_length, hello_param->interface->name);

    /* Imprimo información del paquete HELLO enviado */
    /* Debug("-> PWOSPF: Sending HELLO Packet of length = %d, out of the interface: %s\n", packet_length, hello_param->interface->name);
    Debug("      [Router ID = %s]\n", inet_ntoa(g_router_id)); */
    /*
        Debug("      [Router IP = %s]\n", inet_ntoa(ip));
        Debug("      [Network Mask = %s]\n", inet_ntoa(mask));
    */
/*    Debug("*********************** SALI DEL SEND HELLO PACKET ***********************************\n"); */
    return NULL;
} /* -- send_hello_packet -- */

/*---------------------------------------------------------------------
 * Method: send_all_lsu
 *
 * Construye y envía LSUs cada 30 segundos
 *
 *---------------------------------------------------------------------*/

void *send_all_lsu(void *arg)
{
    struct sr_instance *sr = (struct sr_instance *)arg;

    /* while true*/
    while (1)
    {
        /* Se ejecuta cada OSPF_DEFAULT_LSUINT segundos */
        usleep(OSPF_DEFAULT_LSUINT * 1000000);

        /* Bloqueo para evitar mezclar el envío de HELLOs y LSUs */
        pwospf_lock(sr->ospf_subsys);

        struct sr_if *if_iter = sr->if_list;
        /* Recorro todas las interfaces para enviar el paquete LSU */
        while (if_iter != NULL)
        {
            /* Si la interfaz tiene un vecino, envío un LSU */
            if (if_iter->neighbor_id != 0)
            {
                Debug("\n\nPWOSPF: Sending LSU packet for interface %s: \n", if_iter->name);
                powspf_hello_lsu_param_t *lsu_param = (powspf_hello_lsu_param_t *)(malloc(sizeof(powspf_hello_lsu_param_t)));
                lsu_param->sr = sr;
                lsu_param->interface = if_iter;
                send_lsu(lsu_param);
                
            }
           if_iter = if_iter->next;
        }
        g_sequence_num++;
        /* Desbloqueo */
        pwospf_unlock(sr->ospf_subsys);
    };
    
    return NULL;
} /* -- send_all_lsu -- */

/*---------------------------------------------------------------------
 * Method: send_lsu
 *
 * Construye y envía paquetes LSU a través de una interfaz específica
 *
 *---------------------------------------------------------------------*/

void *send_lsu(void *arg)
{
    Debug("*********************** ENTRE AL SEND LSU PACKET ***********************************\n");
    powspf_hello_lsu_param_t *lsu_param = ((powspf_hello_lsu_param_t *)(arg));
    sr_print_routing_table(lsu_param->sr);
    /* Solo envío LSUs si del otro lado hay un router*/

    /* Construyo el LSU */
    Debug("\n\nPWOSPF: Constructing LSU packet\n");
    sr_ethernet_hdr_t *ethernet_header = ((sr_ethernet_hdr_t *)(malloc(sizeof(sr_ethernet_hdr_t))));
    sr_ip_hdr_t *ip_header = ((sr_ip_hdr_t *)(malloc(sizeof(sr_ip_hdr_t))));
    ospfv2_hdr_t *ospf_header = ((ospfv2_hdr_t *)(malloc(sizeof(ospfv2_hdr_t))));
    ospfv2_lsu_hdr_t *ospf_lsu_header = ((ospfv2_lsu_hdr_t *)(malloc(sizeof(ospfv2_lsu_hdr_t))));
    /* Inicializo cabezal Ethernet */
    /* Dirección MAC destino la dejo para el final ya que hay que hacer ARP */
    ethernet_header->ether_type = htons(ethertype_ip);
    
    int i;
    /* Seteo la dirección MAC origen con la dirección de mi interfaz de salida */
    for (i = 0; i < ETHER_ADDR_LEN; i++)
    {
        ethernet_header->ether_shost[i] = ((uint8_t)(lsu_param->interface->addr[i]));
    }
    uint32_t route_qty = count_routes(lsu_param->sr);

    /* Inicializo cabezal IP*/
    ip_header->ip_v = 4;                                                                              /* Versión IPv4 */
    ip_header->ip_hl = 5;                                                                             /* Longitud del encabezado IP */
    ip_header->ip_tos = 0;                                                                            /* Tipo de servicio */
    ip_header->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (route_qty * sizeof(ospfv2_lsa_t))); /* Longitud total */
    ip_header->ip_id = rand();         /* Identificación única */
    ip_header->ip_off = htons(0x4000); /* Fragmento */
    ip_header->ip_ttl = 64;            /* Tiempo de vida (TTL) */
    /* Seteo el protocolo en el cabezal IP para ser el de OSPF (89) */
    ip_header->ip_p = 89;

    /* Seteo IP origen con la IP de mi interfaz de salida */
    ip_header->ip_src = lsu_param->interface->ip;

    /* La IP destino es la del vecino contectado a mi interfaz*/
    ip_header->ip_dst = lsu_param->interface->neighbor_ip;

    /* Calculo y seteo el chechsum IP*/
    ip_header->ip_sum = 0;
    ip_header->ip_sum = ip_cksum(ip_header, sizeof(sr_ip_hdr_t));

    /* Inicializo cabezal de OSPF*/
    ospf_header->version = OSPF_V2;    /* Versión de OSPFv2 */
    ospf_header->type = OSPF_TYPE_LSU; /* Tipo LSU */

    /* Seteo el Router ID con mi ID */
    ospf_header->rid = g_router_id.s_addr;

    /* Seteo el Area ID en 0 */
    ospf_header->aid = htonl(0);

    /* Seteo el Authentication Type y Authentication Data en 0 */
    ospf_header->autype = 0; /* Tipo de autenticación */
    ospf_header->audata = 0; /* Datos de autenticación */

    /* Seteo el número de secuencia y avanzo*/
    uint16_t sequence_num = g_sequence_num;
    ospf_lsu_header->seq = sequence_num;
    
    /* Seteo el TTL en 64 y el resto de los campos del cabezal de LSU */
    ospf_lsu_header->ttl = 64;
    ospf_lsu_header->unused = 0;
    /* Seteo el número de anuncios con la cantidad de rutas a enviar. Uso función count_routes */
    ospf_lsu_header->num_adv = route_qty;

    /* Creo el paquete y seteo todos los cabezales del paquete a transmitir */
    uint32_t packet_length = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (route_qty * sizeof(ospfv2_lsa_t));
    uint8_t *send_packet = ((uint8_t *)(malloc(packet_length)));
    
    /* Se agregan las cabeceras Ethernet, IP, OSPF y Hello en el orden adecuado. */
    memcpy(send_packet, ethernet_header, sizeof(sr_ethernet_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t), ip_header, sizeof(sr_ip_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t), ospf_header, sizeof(ospfv2_hdr_t));
    memcpy(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t), ospf_lsu_header, sizeof(ospfv2_lsu_hdr_t));

    /* Creo cada LSA iterando en las enttadas de la tabla */
    struct sr_rt *route = lsu_param->sr->routing_table; /*  Accede a la tabla de rutas */
    int lsa_index = 0;                                  /*  Índice para las LSAs dentro del paquete LSU */
    /*Asigno la memoria para todos los LSA*/
    
    while (route != NULL)
    {
        /* Solo envío entradas directamente conectadas y agreagadas a mano*/
        if (route->admin_dst <= 1){
            ospfv2_lsa_t *lsa = ((ospfv2_lsa_t *)(malloc(sizeof(ospfv2_lsa_t))));
            
            /* Creo LSA con subnet, mask y routerID (id del vecino de la interfaz)*/
            lsa->subnet = route->dest.s_addr;                      /*  Dirección de red (subnet) */
            lsa->mask = route->mask.s_addr;                        /*  Máscara de red */
            lsa->rid = sr_get_interface(lsu_param->sr, route->interface)->neighbor_id;         /*  ID del vecino conectado (OBS: Revisar bien esto) */

            memcpy(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (lsa_index * sizeof(ospfv2_lsa_t)), lsa, sizeof(ospfv2_lsa_t));

            lsa_index++;
        }
        route = route->next;
    }

    /* Calculo y actualizo el checksum del cabezal OSPF */
    ((ospfv2_hdr_t *)(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)))->csum = ospfv2_cksum((ospfv2_hdr_t *)(send_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)), sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (route_qty * sizeof(ospfv2_lsa_t)));

    /* Me falta la MAC para poder enviar el paquete, la busco en la cache ARP*/
    Debug("Neighbor IP: ");
    print_addr_ip_int(ntohl(lsu_param->interface->neighbor_ip));
    print_hdr_ospf((uint8_t *)ospf_header);
    /* Envío el paquete si obtuve la MAC o lo guardo en la cola para cuando tenga la MAC*/
    struct sr_arpentry *arp_entry = sr_arpcache_lookup(&lsu_param->sr->cache, lsu_param->interface->neighbor_ip);
    if (arp_entry)
    {
        /* Reenviar el paquete si la dirección MAC está disponible */
        fprintf(stdout, "Enviar el paquete LSU.\n");
        sr_forward_packet(lsu_param->sr, send_packet, packet_length, arp_entry->mac, lsu_param->interface->name);
        /*free(arp_entry);*/
    }
    else
    {
        /* Solicitar ARP si no se conoce la dirección MAC */
        /* fprintf(stdout, "Solicitando ARP para la dirección %u.\n", ntohl(lsu_param->interface->neighbor_ip));*/
        struct sr_arpreq *req = sr_arpcache_queuereq(&lsu_param->sr->cache, lsu_param->interface->neighbor_ip, send_packet, packet_length, lsu_param->interface->name);
        handle_arpreq(lsu_param->sr, req);
    }

    /* Libero memoria */
    /* free(send_packet); */
    Debug("*********************** SALI DEL SEND LSU PACKET ***********************************\n");
    return NULL;
} /* -- send_lsu -- */

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_hello_packet
 *
 * Gestiona los paquetes HELLO recibidos
 *
 *---------------------------------------------------------------------*/

void sr_handle_pwospf_hello_packet(struct sr_instance *sr, uint8_t *packet, unsigned int length, struct sr_if *rx_if)
{
    /* fprintf(stdout, "\n\nPWOSPF: Recibiendo HELLO Packet\n"); */

    /* Debug("*********************** ENTRE AL HANDLE HELLO PACKET ***********************************\n"); */
    int valid = is_packet_valid(packet, length);

    if (valid == 0)
    {
        Debug("-> PWOSPF: HELLO Packet dropped, invalid packet\n");
        return;
    }

    /* Obtengo información del paquete recibido */

    /* Se obtiene el encabezado IP del paquete */
    sr_ip_hdr_t *ip_header = ((sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t)));

    /* Se obtiene el encabezado OSPFv2 */
    ospfv2_hdr_t *ospfv2_header = ((ospfv2_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));

    /* Se obtiene el encabezado HELLO de OSPFv2 */
    ospfv2_hello_hdr_t *ospfv2_hello_header = ((ospfv2_hello_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t)));

    /* Imprimo info del paquete recibido  NO SE SI ES NECESARIO*/
    /* Debug("-> PWOSPF: Detecting PWOSPF HELLO Packet from:\n"); */
    /*
     Debug("      [Neighbor ID = %s]\n", inet_ntoa(ospfv2_header->rid));          // neighbor_id
     Debug("      [Neighbor IP = %s]\n", inet_ntoa(ip_header->ip_src));           // neighbor_ip
     Debug("      [Network Mask = %s]\n", inet_ntoa(ospfv2_hello_header->nmask)); // net_mask
    */

    /* Debug("      [Network Mask Izq = %d]\n", ospfv2_hello_header->nmask);
    Debug("      [Network Mask Der = %d]\n", rx_if->mask); */
    print_addr_ip_int(ospfv2_hello_header->nmask);
    print_addr_ip_int(rx_if->mask);

    /* Chequeo de la máscara de red */
    if (ospfv2_hello_header->nmask != rx_if->mask)
    {
        /* Si la máscara de red no coincide con la de la interfaz, se descarta el paquete HELLO */
        /* Debug("-> PWOSPF: HELLO Packet dropped, invalid hello network mask\n"); */
        return;
    }

    /* Chequeo del intervalo de HELLO */
    if (ospfv2_hello_header->helloint != htons(OSPF_DEFAULT_HELLOINT))
    {
        /* Si el intervalo de HELLO no es el esperado, se descarta el paquete */
        /* Debug("-> PWOSPF: HELLO Packet dropped, invalid hello interval\n"); */
        return;
    }

    /* Seteo el vecino en la interfaz por donde llegó y actualizo la lista de vecinos */
    struct in_addr neighbor_id;
    neighbor_id.s_addr = ospfv2_header->rid;
    
    if (rx_if->neighbor_id != ospfv2_header->rid)
    {
        rx_if->neighbor_id = ospfv2_header->rid;
        rx_if->neighbor_ip = ip_header->ip_src;

        struct ospfv2_neighbor *new_neighbor = create_ospfv2_neighbor(neighbor_id);

        add_neighbor(g_neighbors, new_neighbor);

        /* Si es un nuevo vecino, debo enviar LSUs por todas mis interfaces*/
        powspf_hello_lsu_param_t *lsu_param = ((powspf_hello_lsu_param_t*)(malloc(sizeof(powspf_hello_lsu_param_t))));
        lsu_param->sr = sr;
        struct sr_if *interface = sr->if_list;
        
        pwospf_lock(sr->ospf_subsys);
        /* Recorro todas las interfaces para enviar el paquete LSU */
        while (interface!=NULL)
        {
            /* Si la interfaz tiene un vecino, envío un LSU */
            if (interface->neighbor_id != 0) 
            {
                lsu_param->interface = interface;
                send_lsu(lsu_param);
            }

            interface = interface->next;
        }

        g_sequence_num++;
        
        pwospf_unlock(sr->ospf_subsys);
    }


    refresh_neighbors_alive(g_neighbors, neighbor_id);
    /* Debug("*********************** SALI DEL HANDLE HELLO PACKET ***********************************\n"); */
} /* -- sr_handle_pwospf_hello_packet -- */

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_lsu_packet
 *
 * Gestiona los paquetes LSU recibidos y actualiza la tabla de topología
 * y ejecuta el algoritmo de Dijkstra
 *
 *---------------------------------------------------------------------*/

void *sr_handle_pwospf_lsu_packet(void *arg)
{
    Debug("*********************** ENTRE AL HANDLE LSU PACKET ***********************************\n");
    powspf_rx_lsu_param_t *rx_lsu_param = ((powspf_rx_lsu_param_t *)(arg));
    uint8_t *packet = rx_lsu_param->packet;
    unsigned int length = rx_lsu_param->length;

    fprintf(stdout, "\n\nPWOSPF: Recibiendo LSU Packet\n");

    int valid = is_packet_valid(packet, length);

    if (valid == 0)
    {
        Debug("-> PWOSPF: LSU Packet dropped, invalid packet\n");
        return NULL;
    }

    /* Obtengo información del paquete recibido */

    /* Se obtiene el encabezado IP del paquete */
    sr_ip_hdr_t *ip_header = ((sr_ip_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t)));

    /* Se obtiene el encabezado OSPFv2 */
    ospfv2_hdr_t *ospfv2_header = ((ospfv2_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));

    /* Se obtiene el encabezado LSU de OSPFv2 */
    ospfv2_lsu_hdr_t *ospfv2_lsu_header = ((ospfv2_lsu_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t)));

    /* Obtengo el vecino que me envió el LSU*/
    /* uint32_t neighbor_ip = rx_lsu_param->rx_if->neighbor_ip; */

    /* Imprimo info del paquete recibido*/
    /*
    Debug("-> PWOSPF: Detecting LSU Packet from [Neighbor ID = %s, IP = %s]\n", inet_ntoa(next_hop_id), inet_ntoa(next_hop_ip));
    */

    /* Chequeo checksum */
    /*Debug("-> PWOSPF: LSU Packet dropped, invalid checksum\n");*/
    /*Entiendo que esto no se hace pq lo hace el is_packet_valid()*/

    /* Obtengo el Router ID del router originario del LSU y chequeo si no es mío*/
    if (ospfv2_header->rid == g_router_id.s_addr)
    {
        Debug("-> PWOSPF: LSU Packet dropped, originated by this router\n");
        return NULL;        
    }
    struct in_addr rid_addr;
    rid_addr.s_addr = ospfv2_header->rid;

    Debug("\n============================================== Sequence Number: %d =========================================================\n", ospfv2_lsu_header->seq);

    /* Obtengo el número de secuencia y uso check_sequence_number para ver si ya lo recibí desde ese vecino*/
    int check = check_sequence_number(g_topology,rid_addr,ospfv2_lsu_header->seq);
    Debug(" -------------------- Check: %d\n", check);
    if (check == 0) {
        Debug("-> PWOSPF: LSU Packet dropped, repeated sequence number\n");
        return NULL; 
    } 

    /* Itero en los LSA que forman parte del LSU. Para cada uno, actualizo la topología.*/

    int lsa_index = 0;       /* Índice para las LSAs dentro del paquete LSU */
    while (lsa_index < ospfv2_lsu_header->num_adv)
    {
        Debug("-> PWOSPF: Processing LSAs and updating topology table\n");

        /* Obtén un puntero a la ubicación del LSA en el paquete */
        ospfv2_lsa_t *lsa = (ospfv2_lsa_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (lsa_index * sizeof(ospfv2_lsa_t)));

        /*
        Debug("      [Subnet = %s]", inet_ntoa(net_num));
        Debug("      [Mask = %s]", inet_ntoa(net_mask));
        Debug("      [Neighbor ID = %s]\n", inet_ntoa(neighbor_id));
        */
        struct in_addr subnet_addr, mask_addr, lsa_rid_addr, ip_src_addr;

        
        subnet_addr.s_addr = lsa->subnet;
        mask_addr.s_addr = lsa->mask;
        lsa_rid_addr.s_addr = lsa->rid;
        ip_src_addr.s_addr = ip_header->ip_src;
        
        refresh_topology_entry(g_topology,rid_addr,subnet_addr,mask_addr,lsa_rid_addr,ip_src_addr,ospfv2_lsu_header->seq);

        /* Incrementa el índice de LSAs */
        lsa_index++;
    }

    /* Imprimo la topología */
    Debug("\n-> PWOSPF: Printing the topology table\n");
    print_topolgy_table(g_topology);
    

    /* Ejecuto Dijkstra en un nuevo hilo (run_dijkstra) */
    dijkstra_param_t *dijkstra_data = ((dijkstra_param_t*)(malloc(sizeof(dijkstra_param_t))));
    
    dijkstra_data->sr = rx_lsu_param->sr;
    dijkstra_data->topology = g_topology;
    dijkstra_data->rid = g_router_id;
    dijkstra_data->mutex = g_dijkstra_mutex;

    pthread_create(&g_dijkstra_thread, NULL, run_dijkstra, dijkstra_data);

    struct sr_if* interface = rx_lsu_param->sr->if_list;

    sr_print_routing_table(rx_lsu_param->sr);

    /* Flooding del LSU por todas las interfaces menos por donde me llegó */
    while (interface!=NULL)
    {
        /* solo reenvio si no es la interfaz de origen */
        if (interface->addr != rx_lsu_param->rx_if->addr) /* No se si es la mejor manera de fijarse que sea la interfaz de origen */
        {
            int i;
            /* Seteo MAC de origen */
            for (i = 0; i < ETHER_ADDR_LEN; i++)
            {
                ((sr_ethernet_hdr_t*)(packet))->ether_shost[i] = ((uint8_t)(interface->addr[i]));
            }
            /* Ajusto paquete IP, origen y checksum*/
            ip_header->ip_src = interface->ip;
            ip_header->ip_sum = 0;
            ip_header->ip_sum = ip_cksum(ip_header, sizeof(sr_ip_hdr_t));
            /* Ajusto cabezal OSPF: checksum y TTL*/
            ospfv2_lsu_header->ttl--;
            ospfv2_header->csum = ospfv2_cksum( ospfv2_header, sizeof(ospfv2_hdr_t) + sizeof(ospfv2_lsu_hdr_t) + (ospfv2_lsu_header->num_adv * sizeof(ospfv2_lsa_t)) );

            /* Envío el paquete*/
            struct sr_arpentry *arp_entry = sr_arpcache_lookup(&rx_lsu_param->sr->cache, interface->neighbor_ip);
            if (arp_entry)
            {
                /* Reenviar el paquete si la dirección MAC está disponible */
                fprintf(stdout, "Enviar el paquete LSU.\n");
                sr_forward_packet(rx_lsu_param->sr, packet, rx_lsu_param->length, arp_entry->mac, interface->name);
                /*free(arp_entry);*/
            }
            else
            {
                /* Solicitar ARP si no se conoce la dirección MAC */
                /* fprintf(stdout, "Solicitando ARP para la dirección %u.\n", ntohl(rx_lsu_param->interface->neighbor_ip));*/
                struct sr_arpreq *req = sr_arpcache_queuereq(&rx_lsu_param->sr->cache, interface->neighbor_ip, packet, rx_lsu_param->length, interface->name);
                handle_arpreq(rx_lsu_param->sr, req);
            }
        }

        interface = interface->next;
    }
    

    /* -------------------------- */
    /* No se si esto de aca abajo es necesario o no. Principalmente por el TTL */

    /* Seteo MAC de origen */
    /* Ajusto paquete IP, origen y checksum*/
    /* Ajusto cabezal OSPF: checksum y TTL*/
    /* Envío el paquete*/
    Debug("******************************************************** SALI DEL HANDLE LSU PACKET ********************************************************\n\n\n");
    return NULL;
} /* -- sr_handle_pwospf_lsu_packet -- */

/**********************************************************************************
 * SU CÓDIGO DEBERÍA TERMINAR AQUÍ
 * *********************************************************************************/

/*---------------------------------------------------------------------
 * Method: sr_handle_pwospf_packet
 *
 * Gestiona los paquetes PWOSPF
 *
 *---------------------------------------------------------------------*/

void sr_handle_pwospf_packet(struct sr_instance *sr, uint8_t *packet, unsigned int length, struct sr_if *rx_if)
{
    Debug("******************************************************** ENTRANDO EN HANDLE PWOSPF PACKET ********************************************************\n\n\n");
    /*Si aún no terminó la inicialización, se descarta el paquete recibido*/
    if (g_router_id.s_addr == 0)
    {
        return;
    }

    ospfv2_hdr_t *rx_ospfv2_hdr = ((ospfv2_hdr_t *)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t)));
    powspf_rx_lsu_param_t *rx_lsu_param = ((powspf_rx_lsu_param_t *)(malloc(sizeof(powspf_rx_lsu_param_t))));

    Debug("-> PWOSPF: Detecting PWOSPF Packet\n");
    Debug("      [Type = %d]\n", rx_ospfv2_hdr->type);

    switch (rx_ospfv2_hdr->type)
    {
    case OSPF_TYPE_HELLO:
        sr_handle_pwospf_hello_packet(sr, packet, length, rx_if);
        break;
    case OSPF_TYPE_LSU:
        rx_lsu_param->sr = sr;
        unsigned int i;
        for (i = 0; i < length; i++)
        {
            rx_lsu_param->packet[i] = packet[i];
        }
        rx_lsu_param->length = length;
        rx_lsu_param->rx_if = rx_if;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_t pid;
        pthread_create(&pid, &attr, sr_handle_pwospf_lsu_packet, rx_lsu_param);
        break;
    }
} /* -- sr_handle_pwospf_packet -- */
