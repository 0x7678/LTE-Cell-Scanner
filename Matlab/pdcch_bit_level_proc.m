function pdcch_info = pdcch_bit_level_proc(peak, slot_idx, pdcch_sym_deinterleave, pdcch_ce_deinterleave, np_ce)
n_ports = peak.n_ports;
n_id_cell = peak.n_id_cell;

c_init = floor(slot_idx/2)*(2^9) + n_id_cell;
np_mean = mean(np_ce);

num_quadruplet = size(pdcch_sym_deinterleave, 1);

pdcch_sym_all = pdcch_sym_deinterleave.';
pdcch_sym_all = pdcch_sym_all(:).';

pdcch_ce_all = zeros(n_ports, num_quadruplet*4);
for i = 1 : n_ports
    tmp_ce = pdcch_ce_deinterleave(:,:,i);
    tmp_ce = tmp_ce.';
    tmp_ce = tmp_ce(:).';
    pdcch_ce_all(i,:) = tmp_ce;
end

% -------blind search -----------
N_REG = length(pdcch_sym_all)/4;
N_CCE = floor(N_REG/9);

num_CCE = 16;  % common search space
L_set = [4 8]; % common search space
bits_set = [288 576];
Y = 0;         % common search space
pdcch_info = [];
for l = 1 : length(L_set)
    num_bits = bits_set(l);
    L = L_set(l);
    M = num_CCE/L;
    for m = 0 : (M-1)
        sc_idx = zeros(L, 36);
        for i = 0 : (L-1)
            CCE_idx = L*( mod( Y+m, floor(N_CCE/L) ) ) + i;
            sc_idx(i+1, :) = CCE_idx*36 : (CCE_idx*36 + 35);
        end
        sc_idx = sc_idx.';
        sc_idx = sc_idx(:).'; 
        
        pdcch_sym = pdcch_sym_all(sc_idx+1);
        pdcch_ce = pdcch_ce_all(:, sc_idx+1);
        
%         plot(abs(sym));
        if (n_ports==1)
    
            gain=conj(pdcch_ce(1,:))./absx2(pdcch_ce(1,:));
            syms=pdcch_sym.*gain;
            np=np_mean*absx2(gain);

        elseif (n_ports==2)

            syms=NaN(1,length(pdcch_sym));
            np=NaN(1,length(pdcch_sym));
            for t=1:2:length(syms)
                % http://en.wikipedia.org/wiki/Space-time_block_coding_based_transmit_diversity
                h1=mean(pdcch_ce(1,t:t+1));
                h2=mean(pdcch_ce(2,t:t+1));
                x1=pdcch_sym(t);
                x2=pdcch_sym(t+1);
                scale=sum(absx2([h1 h2]));
                syms(t)=(conj(h1)*x1+h2*conj(x2))/scale;
                syms(t+1)=conj((-conj(h2)*x1+h1*conj(x2))/scale);
                np(t)=(abs(h1)/scale)^2*np_mean+(abs(h2)/scale)^2*np_mean;
                np(t+1)=np(t);
            end
            % 3dB factor comes from precoding for transmit diversity
            syms=syms*sqrt(2);

        elseif (n_ports==4)

            syms=NaN(1,length(pdcch_sym));
            np=NaN(1,length(pdcch_sym));
            for t=1:2:length(syms)
                % http://en.wikipedia.org/wiki/Space-time_block_coding_based_transmit_diversity
                if (mod(t,4)==1)
                  h1=mean(pdcch_ce(1,t:t+1));
                  h2=mean(pdcch_ce(3,t:t+1));
                else
                  h1=mean(pdcch_ce(2,t:t+1));
                  h2=mean(pdcch_ce(4,t:t+1));
                end
                x1=pdcch_sym(t);
                x2=pdcch_sym(t+1);
                scale=sum(absx2([h1 h2]));
                syms(t)=(conj(h1)*x1+h2*conj(x2))/scale;
                syms(t+1)=conj((-conj(h2)*x1+h1*conj(x2))/scale);
                np(t)=(abs(h1)/scale)^2*np_mean+(abs(h2)/scale)^2*np_mean;
                np(t+1)=np(t);
            end
            % 3dB factor comes from precoding for transmit diversity
            syms=syms*sqrt(2);

        else
            error('Check code...');
        end

        % ------Extract the bits---------
        e_est=deqam(syms,np,'QAM','LTE');

        % ----------Unscramble---------------
        scr=lte_pn(c_init,length(e_est));
        e_est(scr==1)=1-e_est(scr==1);

        % Undo ratematching
        d_est=lte_conv_deratematch(e_est, num_bits);
        % Viterbi decode
        c_est=lte_conv_decode(d_est);
        % Calculate the received CRC
        crc_est=lte_calc_crc(c_est(1:(num_bits-16)),16);
        %c_est(25:end)
        % Apply CRC mask
        if (n_ports==2)
            crc_est=1-crc_est;
        elseif (n_ports==4)
            crc_est(2:2:end)=1-crc_est(2:2:end);
        end
        %crc_est
        a = xor(crc_est, c_est((end-15):end) );

        bits = [dec2hex(bi2de(a)) ' ' dec2hex(bi2de(a(end:-1:1)))  ' ' ] ;
        pdcch_info = [pdcch_info bits];
    end
end



